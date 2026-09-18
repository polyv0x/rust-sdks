// Copyright 2026 LiveKit, Inc.
// Licensed under the Apache License, Version 2.0.

//! Application-owned native decoders. No codec or application IPC dependency lives here.

use std::{
    any::Any,
    sync::{Arc, OnceLock},
};

/// A ref-counted decoded allocation. Dropping its last owner releases the application lease.
pub trait NativeDecodedFrame: Any + Send + Sync {
    /// Coded frame dimensions.
    fn dimensions(&self) -> (u32, u32);
    /// Optional tightly packed I420 compatibility pixels. Must not wait indefinitely.
    fn to_i420(&self) -> Option<Vec<u8>>;
    /// Access the application owner without exposing an unowned native pointer.
    fn as_any(&self) -> &dyn Any;
}

/// Asynchronous decoder interface. Inputs are complete, already-decrypted access units.
pub trait ExternalVideoDecoder: Send {
    /// Configure or reset the decoder. The output is revoked before release returns.
    fn configure(&mut self, config: ffi::ExternalDecoderConfig, output: DecodedCallback) -> bool;
    /// Admit a bounded input, request a keyframe, or switch to the built-in fallback.
    fn decode(&mut self, input: ffi::ExternalEncodedInput) -> ffi::ExternalDecodeStatus;
    /// Cancel output and release decoder work; this must have a bounded completion time.
    fn release(&mut self);
    /// Report the actual decoding implementation, independently of presentation preparation.
    fn info(&self) -> ffi::ExternalDecoderInfo;
}

/// Factory called for a codec's SDP name (H264, AV1, VP8 or VP9).
pub type ExternalDecoderFactory =
    dyn Fn(&str) -> Option<Box<dyn ExternalVideoDecoder>> + Send + Sync;
static FACTORY: OnceLock<Arc<ExternalDecoderFactory>> = OnceLock::new();

/// Install before creating a media runtime. Existing factories/decoders are never retargeted.
pub fn install_external_decoder_factory(
    factory: Arc<ExternalDecoderFactory>,
) -> Result<(), Arc<ExternalDecoderFactory>> {
    FACTORY.set(factory)
}

/// Output capability. Calling after decoder release safely drops the frame without a callback.
#[derive(Clone)]
pub struct DecodedCallback(cxx::SharedPtr<ffi::ExternalDecoderCallback>);
impl DecodedCallback {
    /// Deliver a native frame with the exact input RTP timestamp, preserving WebRTC scheduling.
    pub fn decoded(&self, frame: Arc<dyn NativeDecodedFrame>, rtp_timestamp: u32) {
        let (width, height) = frame.dimensions();
        if width == 0 || height == 0 || width > 16384 || height > 16384 {
            return;
        }
        self.0.decoded(Box::new(ExternalFrameOwner::from_frame(frame)), rtp_timestamp);
    }
}

/// Opaque owner carried by a native C++ VideoFrameBuffer.
pub struct ExternalFrameOwner {
    frame: Option<Arc<dyn NativeDecodedFrame>>,
}
impl ExternalFrameOwner {
    pub(crate) fn from_frame(frame: Arc<dyn NativeDecodedFrame>) -> Self {
        Self { frame: Some(frame) }
    }
    /// Clone the safe owner if the buffer originated from an application decoder.
    pub fn frame(&self) -> Option<Arc<dyn NativeDecodedFrame>> {
        self.frame.clone()
    }
    fn frame_width(&self) -> u32 {
        self.frame.as_ref().map_or(0, |frame| frame.dimensions().0)
    }
    fn frame_height(&self) -> u32 {
        self.frame.as_ref().map_or(0, |frame| frame.dimensions().1)
    }
    fn frame_pixels(&self) -> Vec<u8> {
        self.frame.as_ref().and_then(|frame| frame.to_i420()).unwrap_or_default()
    }
    fn clone_owner(&self) -> Box<Self> {
        Box::new(Self { frame: self.frame.clone() })
    }
}
fn empty_external_frame_owner() -> Box<ExternalFrameOwner> {
    Box::new(ExternalFrameOwner { frame: None })
}

/// Opaque decoder implementation retained by the C++ adapter.
pub struct ExternalDecoderHandle {
    inner: Option<Box<dyn ExternalVideoDecoder>>,
}
fn create_external_decoder(codec: &str) -> Box<ExternalDecoderHandle> {
    Box::new(ExternalDecoderHandle { inner: FACTORY.get().and_then(|factory| factory(codec)) })
}
impl ExternalDecoderHandle {
    fn decoder_available(&self) -> bool {
        self.inner.is_some()
    }
    fn configure_decoder(
        &mut self,
        config: ffi::ExternalDecoderConfig,
        callback: cxx::SharedPtr<ffi::ExternalDecoderCallback>,
    ) -> bool {
        self.inner
            .as_mut()
            .is_some_and(|decoder| decoder.configure(config, DecodedCallback(callback)))
    }
    fn decode_input(&mut self, input: ffi::ExternalEncodedInput) -> ffi::ExternalDecodeStatus {
        self.inner
            .as_mut()
            .map_or(ffi::ExternalDecodeStatus::Fallback, |decoder| decoder.decode(input))
    }
    fn release_decoder(&mut self) {
        if let Some(decoder) = &mut self.inner {
            decoder.release();
        }
    }
    fn decoder_info(&self) -> ffi::ExternalDecoderInfo {
        self.inner.as_ref().map_or_else(
            || ffi::ExternalDecoderInfo { name: "Unavailable".into(), hardware: false },
            |decoder| decoder.info(),
        )
    }
}

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    /// Admission outcome; recovery never pretends that dropped reference inputs decoded.
    enum ExternalDecodeStatus {
        Accepted,
        NeedKeyframe,
        Fallback,
    }
    /// Decoder resource limits supplied by libwebrtc.
    struct ExternalDecoderConfig {
        max_width: i32,
        max_height: i32,
        cores: i32,
    }
    /// An assembled encoded access unit, after WebRTC decryption.
    struct ExternalEncodedInput {
        data: Vec<u8>,
        rtp_timestamp: u32,
        keyframe: bool,
        render_time_ms: i64,
    }
    /// Truthful backend telemetry returned to libwebrtc stats.
    struct ExternalDecoderInfo {
        name: String,
        hardware: bool,
    }

    extern "Rust" {
        type ExternalDecoderHandle;
        type ExternalFrameOwner;
        fn create_external_decoder(codec: &str) -> Box<ExternalDecoderHandle>;
        fn decoder_available(self: &ExternalDecoderHandle) -> bool;
        fn configure_decoder(
            self: &mut ExternalDecoderHandle,
            config: ExternalDecoderConfig,
            callback: SharedPtr<ExternalDecoderCallback>,
        ) -> bool;
        fn decode_input(
            self: &mut ExternalDecoderHandle,
            input: ExternalEncodedInput,
        ) -> ExternalDecodeStatus;
        fn release_decoder(self: &mut ExternalDecoderHandle);
        fn decoder_info(self: &ExternalDecoderHandle) -> ExternalDecoderInfo;
        fn frame_width(self: &ExternalFrameOwner) -> u32;
        fn frame_height(self: &ExternalFrameOwner) -> u32;
        fn frame_pixels(self: &ExternalFrameOwner) -> Vec<u8>;
        fn clone_owner(self: &ExternalFrameOwner) -> Box<ExternalFrameOwner>;
        fn empty_external_frame_owner() -> Box<ExternalFrameOwner>;
    }
    unsafe extern "C++" {
        include!("livekit/external_video_decoder.h");
        type VideoFrameBuffer = crate::video_frame_buffer::ffi::VideoFrameBuffer;
        type ExternalDecoderCallback;
        fn decoded(
            self: &ExternalDecoderCallback,
            frame: Box<ExternalFrameOwner>,
            rtp_timestamp: u32,
        );
        fn external_frame_owner(buffer: &VideoFrameBuffer) -> Box<ExternalFrameOwner>;
    }
    #[cfg(feature = "decoder_tests")]
    unsafe extern "C++" {
        include!("livekit/decoder_tests.h");
        fn external_decoder_roundtrip(frame: Box<ExternalFrameOwner>) -> Box<ExternalFrameOwner>;
        fn external_decoder_revoked(frame: Box<ExternalFrameOwner>);
    }
}

// SAFETY: the callback's C++ mutex serializes completion and revocation. Its methods retain
// owners and never expose unowned frame pointers. Revoked callbacks only drop their arguments.
unsafe impl Send for ffi::ExternalDecoderCallback {}
// SAFETY: same mutex protects all shared callback state, including the decoded callback pointer.
unsafe impl Sync for ffi::ExternalDecoderCallback {}
