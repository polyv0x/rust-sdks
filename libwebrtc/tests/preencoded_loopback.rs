// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0

//! Local RTP/E2EE regression for the encoded-only source and real publisher.
//! No room credentials, screen capture, STUN service, or user content is used.

#![cfg(not(target_arch = "wasm32"))]

use std::time::Duration;

use libwebrtc::{
    ice_candidate::IceCandidate,
    media_stream_track::MediaStreamTrack,
    native::frame_cryptor::{
        EncodedVideoFrame as ReceivedFrame, EncryptionAlgorithm, FrameCryptor,
        KeyDerivationAlgorithm, KeyProvider, KeyProviderOptions,
    },
    peer_connection::{AnswerOptions, OfferOptions, PeerConnection, PeerConnectionState},
    peer_connection_factory::{
        native::PeerConnectionFactoryExt, PeerConnectionFactory, RtcConfiguration,
    },
    rtp_sender::VideoEncoderBackend,
    rtp_transceiver::{RtpTransceiverDirection, RtpTransceiverInit},
    video_frame::{
        EncodedFrameType, EncodedVideoCodec, EncodedVideoFrame, I420Buffer, VideoFrame,
        VideoRotation,
    },
    video_source::{native::NativeVideoSource, VideoResolution},
    MediaType,
};
use tokio::{sync::mpsc, time};

// Synthetic gray 128x128/30fps, FFmpeg libx264 constrained baseline, no B frames,
// repeat-headers=1, keyint=30, scenecut=0, with SEI and AUD removed.
// All NAL prefixes are normalized to four bytes, as in the native capture
// worker: the H.264 frame cryptor requires consistent Annex-B framing.
const H264_KEY_HEX: &str = concat!(
    "000000016742c00bd902046c0440000003004000000f03c50a92",
    "0000000168cb83cb20",
    "000000016588840af2628001793939393939393a",
    "ebaebaebaebaebaebaebaebaebaebaebaebaebaebaebaebaebaebaebaeba",
    "ebaebaebaebaebaebaebaebc",
);
const H264_DELTA_HEX: &str = "00000001419a3813e04180";

// Synthetic gray 128x128 L1T1, SVT-AV1 4.2.0; pred-struct=1, hierarchical-levels=0.
const AV1_KEY_HEX: &str = "12000a0b0200000419bfff12be4010320f100085a004102080000000ec895ca8";
const AV1_DELTA_HEX: &str = "1200321230023f400000224600000080008000945f40";

/// Closes native connections on success, timeout, and assertion failure.
struct Peers {
    publisher: PeerConnection,
    subscriber: PeerConnection,
}

impl Drop for Peers {
    fn drop(&mut self) {
        self.publisher.close();
        self.subscriber.close();
    }
}

/// Decodes small, synthetic test fixtures without introducing a dependency.
fn from_hex(hex: &str) -> Vec<u8> {
    assert_eq!(hex.len() % 2, 0);
    (0..hex.len())
        .step_by(2)
        .map(|offset| u8::from_str_radix(&hex[offset..offset + 2], 16).unwrap())
        .collect()
}

/// Extracts coded slices, allowing the RTP receiver to rewrite SPS VUI metadata.
fn h264_slices(bytes: &[u8]) -> Vec<&[u8]> {
    let starts: Vec<_> = bytes
        .windows(3)
        .enumerate()
        .filter_map(|(index, prefix)| (prefix == [0, 0, 1]).then_some(index))
        .collect();
    starts
        .iter()
        .enumerate()
        .filter_map(|(index, &start)| {
            let mut end = starts.get(index + 1).copied().unwrap_or(bytes.len());
            while end > start + 3 && bytes[end - 1] == 0 {
                end -= 1;
            }
            let nal = &bytes[start + 3..end];
            nal.first().is_some_and(|byte| matches!(byte & 31, 1 | 5)).then_some(nal)
        })
        .collect()
}

/// Collects ICE candidates without panicking on native callback threads at teardown.
fn candidates(peer: &PeerConnection) -> mpsc::UnboundedReceiver<IceCandidate> {
    let (tx, rx) = mpsc::unbounded_channel();
    peer.on_ice_candidate(Some(Box::new(move |candidate| {
        let _ = tx.send(candidate);
    })));
    rx
}

/// Negotiates a pair and relays only local host candidates until DTLS connects.
async fn connect(peers: &Peers) {
    let mut publisher_ice = candidates(&peers.publisher);
    let mut subscriber_ice = candidates(&peers.subscriber);
    let offer = peers.publisher.create_offer(OfferOptions::default()).await.unwrap();
    peers.publisher.set_local_description(offer.clone()).await.unwrap();
    peers.subscriber.set_remote_description(offer).await.unwrap();
    let answer = peers.subscriber.create_answer(AnswerOptions::default()).await.unwrap();
    peers.subscriber.set_local_description(answer.clone()).await.unwrap();
    peers.publisher.set_remote_description(answer).await.unwrap();
    while peers.publisher.connection_state() != PeerConnectionState::Connected
        || peers.subscriber.connection_state() != PeerConnectionState::Connected
    {
        tokio::select! {
            Some(candidate) = publisher_ice.recv() => {
                peers.subscriber.add_ice_candidate(candidate).await.unwrap();
            }
            Some(candidate) = subscriber_ice.recv() => {
                peers.publisher.add_ice_candidate(candidate).await.unwrap();
            }
            _ = time::sleep(Duration::from_millis(10)) => {}
        }
    }
}

/// Publishes synthetic access units through the actual encoder queue, RTP, and cryptor.
async fn round_trip(
    codec: EncodedVideoCodec,
    backend: VideoEncoderBackend,
    encrypted: bool,
    raw_pixels: bool,
    replace_transformers: bool,
) {
    let _ = env_logger::builder().is_test(true).try_init();
    let factory = PeerConnectionFactory::default();
    let peers = Peers {
        publisher: factory.create_peer_connection(RtcConfiguration::default()).unwrap(),
        subscriber: factory.create_peer_connection(RtcConfiguration::default()).unwrap(),
    };
    let resolution = VideoResolution { width: 128, height: 128 };
    let source = if raw_pixels {
        NativeVideoSource::new(resolution.clone(), false)
    } else {
        NativeVideoSource::new_encoded(resolution.clone())
    };
    let track = factory.create_video_track("synthetic-first-frame-regression", source.clone());
    let transceiver = peers
        .publisher
        .add_transceiver(
            MediaStreamTrack::Video(track),
            RtpTransceiverInit {
                direction: RtpTransceiverDirection::SendOnly,
                stream_ids: vec!["local-test".into()],
                send_encodings: vec![],
            },
        )
        .unwrap();
    let mime = match codec {
        EncodedVideoCodec::H264 => "video/H264",
        EncodedVideoCodec::AV1 => "video/AV1",
        _ => panic!("unsupported fixture codec"),
    };
    let codecs: Vec<_> = factory
        .get_rtp_sender_capabilities(MediaType::Video)
        .codecs
        .into_iter()
        .filter(|capability| {
            capability.mime_type.eq_ignore_ascii_case(mime)
                && (codec != EncodedVideoCodec::H264
                    || capability.sdp_fmtp_line.as_deref().is_some_and(|fmtp| {
                        fmtp.contains("profile-level-id=42e01f")
                            && fmtp.contains("packetization-mode=1")
                    }))
        })
        .collect();
    assert!(!codecs.is_empty(), "required codec unavailable: {mime}");
    transceiver.set_codec_preferences(codecs).unwrap();
    transceiver.sender().set_video_encoder_backend(backend);
    connect(&peers).await;

    let provider = KeyProvider::new(KeyProviderOptions {
        shared_key: true,
        ratchet_window_size: 16,
        ratchet_salt: b"synthetic-first-frame-test".to_vec(),
        failure_tolerance: -1,
        key_ring_size: 16,
        key_derivation_algorithm: KeyDerivationAlgorithm::HKDF,
    });
    assert!(provider.set_shared_key(0, vec![42; 32]));
    let mut sender_cryptor = FrameCryptor::new_for_rtp_sender(
        &factory,
        "synthetic-publisher".into(),
        EncryptionAlgorithm::AesGcm,
        provider.clone(),
        transceiver.sender(),
    );
    let receiver_cryptor = FrameCryptor::new_for_rtp_receiver(
        &factory,
        "synthetic-publisher".into(),
        EncryptionAlgorithm::AesGcm,
        provider.clone(),
        peers.subscriber.receivers().into_iter().next().expect("video receiver"),
    );
    sender_cryptor.set_enabled(encrypted);
    receiver_cryptor.set_enabled(encrypted);
    sender_cryptor.on_state_change(Some(Box::new(|_, state| {
        eprintln!("sender cryptor: {state:?}");
    })));
    receiver_cryptor.on_state_change(Some(Box::new(|_, state| {
        eprintln!("receiver cryptor: {state:?}");
    })));
    let (frames_tx, mut frames_rx) = mpsc::channel::<ReceivedFrame>(64);
    receiver_cryptor.on_encoded_video_frame(Some(Box::new(move |frame| {
        let _ = frames_tx.try_send(frame);
    })));

    let (key, delta) = if codec == EncodedVideoCodec::H264 {
        (from_hex(H264_KEY_HEX), from_hex(H264_DELTA_HEX))
    } else {
        (from_hex(AV1_KEY_HEX), from_hex(AV1_DELTA_HEX))
    };
    let mut cadence = time::interval(Duration::from_millis(34));
    cadence.set_missed_tick_behavior(time::MissedTickBehavior::Skip);
    let mut submitted = 0_i64;
    let mut received = 0;
    let mut received_key = false;
    let mut received_delta = false;
    let mut previous_timestamp = None;
    let mut bitrate_feedback = raw_pixels;
    let mut raw_frame =
        VideoFrame::new(VideoRotation::VideoRotation0, I420Buffer::new_black(128, 128));
    while received < 6
        || !received_key
        || !received_delta
        || !bitrate_feedback
        || (replace_transformers && submitted < 600)
    {
        tokio::select! {
            _ = cadence.tick() => {
                // Alternate a real IDR and its corresponding inter frame. This
                // keeps decoding valid during normal startup drops or a PLI.
                let keyframe = submitted % 2 == 0 || source.take_keyframe_request();
                let timestamp_us = 1_000_000 + submitted * 34_000;
                if raw_pixels {
                    raw_frame.timestamp_us = timestamp_us;
                    source.capture_frame(&raw_frame);
                } else {
                    assert!(source.capture_encoded_frame(&EncodedVideoFrame {
                        codec,
                        payload: if keyframe { &key } else { &delta },
                        timestamp_us,
                        frame_type: if keyframe { EncodedFrameType::Key } else { EncodedFrameType::Delta },
                        resolution: resolution.clone(),
                        frame_metadata: None,
                    }));
                }
                submitted += 1;
                if replace_transformers && submitted % 15 == 0 {
                    // Reconfigure while transform callbacks can still own
                    // queued frames. Reset must drain those references before
                    // the old RTP sender releases its delegate.
                    sender_cryptor = FrameCryptor::new_for_rtp_sender(
                        &factory, "synthetic-publisher".into(), EncryptionAlgorithm::AesGcm,
                        provider.clone(), transceiver.sender(),
                    );
                    sender_cryptor.set_enabled(encrypted);
                }
                bitrate_feedback |= source.take_rate_control_request().is_some();
                if submitted % 60 == 0 {
                    eprintln!("{mime}: submitted={submitted}, received={received}, key={received_key}, delta={received_delta}, bitrate={bitrate_feedback}");
                }
            }
            Some(frame) = frames_rx.recv() => {
                assert!(frame.mime_type.eq_ignore_ascii_case(mime));
                assert!(!frame.data.is_empty());
                assert_ne!(previous_timestamp, Some(frame.timestamp), "RTP timestamp repeated");
                previous_timestamp = Some(frame.timestamp);
                received_key |= frame.key_frame;
                received_delta |= !frame.key_frame;
                if codec == EncodedVideoCodec::H264 && !raw_pixels {
                    let expected = h264_slices(if frame.key_frame { &key } else { &delta });
                    assert!(!expected.is_empty());
                    assert_eq!(h264_slices(&frame.data), expected, "coded slices changed in transit");
                }
                received += 1;
            }
        }
    }
    receiver_cryptor.on_encoded_video_frame(None);
    eprintln!("{mime}: backend={backend:?}, encrypted={encrypted}, raw={raw_pixels}, submitted={submitted}, received={received}, key+delta verified");
}

/// Covers the initial untagged encoder with no selector available to rescue it.
#[tokio::test]
#[ignore = "opens local peer-to-peer UDP sockets; run explicitly on a native host"]
async fn h264_untagged_publisher_round_trip() {
    time::timeout(
        Duration::from_secs(15),
        round_trip(EncodedVideoCodec::H264, VideoEncoderBackend::Auto, false, false, false),
    )
    .await
    .expect("untagged H.264 publisher stalled");
}

/// Repeats the application's encrypted pre-encoded publish and teardown path.
#[tokio::test]
#[ignore = "opens local peer-to-peer UDP sockets; run explicitly on a native host"]
async fn h264_encrypted_preencoded_publish_restart() {
    for _ in 0..3 {
        time::timeout(
            Duration::from_secs(15),
            round_trip(
                EncodedVideoCodec::H264,
                VideoEncoderBackend::PreEncoded,
                true,
                false,
                false,
            ),
        )
        .await
        .expect("encrypted H.264 publisher stalled");
    }
}

/// Exercises AV1 RTP/E2EE transport; synthetic input does not qualify hardware AV1 capture.
#[tokio::test]
#[ignore = "opens local peer-to-peer UDP sockets; run explicitly on a native host"]
async fn av1_encrypted_preencoded_round_trip() {
    time::timeout(
        Duration::from_secs(15),
        round_trip(EncodedVideoCodec::AV1, VideoEncoderBackend::PreEncoded, true, false, false),
    )
    .await
    .expect("encrypted AV1 publisher stalled");
}

/// Verifies the production factory still encodes ordinary camera-style pixels.
#[tokio::test]
#[ignore = "opens local peer-to-peer sockets and a platform H.264 encoder"]
async fn raw_h264_automatic_encoder_still_publishes() {
    time::timeout(
        Duration::from_secs(15),
        round_trip(EncodedVideoCodec::H264, VideoEncoderBackend::Auto, false, true, false),
    )
    .await
    .expect("raw H.264 publisher stalled");
}

/// Retires senders with in-flight encrypted frames on the real native queues.
#[tokio::test]
#[ignore = "opens local peer-to-peer UDP sockets; run explicitly on a native host"]
async fn encrypted_video_transformer_replacement_drains_queued_frames() {
    for codec in [EncodedVideoCodec::H264, EncodedVideoCodec::AV1] {
        time::timeout(
            Duration::from_secs(30),
            round_trip(codec, VideoEncoderBackend::PreEncoded, true, false, true),
        )
        .await
        .expect("encrypted video transformer replacement stalled");
    }
}
