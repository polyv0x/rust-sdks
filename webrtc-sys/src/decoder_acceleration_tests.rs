// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
//! Tests the native metadata-to-statistics boundary and real VideoToolbox sessions.

#[cxx::bridge(namespace = "livekit_ffi")]
mod ffi {
    // SAFETY: The harness exchanges only owned JSON and borrowed immutable bytes.
    unsafe extern "C++" {
        include!("livekit/decoder_acceleration_tests.h");
        /// Serializes an instrumented, retired, or legacy decoder's statistics.
        fn decoder_acceleration_stats_for_test(scenario: u8) -> Result<String>;
        #[cfg(target_os = "macos")]
        /// Decodes two synthetic keyframes and reports session lifecycle measurements.
        fn apple_h264_acceleration_for_test(
            first: &[u8],
            replacement: &[u8],
        ) -> Result<Vec<String>>;
    }
}

#[test]
fn decoder_acceleration_preserves_hardware_software_and_unknown() {
    for (scenario, expected) in [(0, None), (1, Some(true)), (2, Some(false)), (3, None), (4, None)]
    {
        let json = ffi::decoder_acceleration_stats_for_test(scenario).expect("native report");
        let stats: serde_json::Value = serde_json::from_str(&json).expect("valid statistics");
        assert_eq!(stats[0]["powerEfficientDecoder"].as_bool(), expected);
        assert_eq!(stats[0]["decoderImplementation"], "VideoToolbox");
        assert_eq!(stats[0]["ssrc"], 42);
        assert_eq!(stats[0]["framesDecoded"], 7);
        assert_eq!(stats[0]["id"], "video");
    }
}

#[test]
fn decoder_acceleration_does_not_mix_concurrent_sessions() {
    let json = ffi::decoder_acceleration_stats_for_test(5).expect("native report");
    let stats: serde_json::Value = serde_json::from_str(&json).expect("valid statistics");
    let reports = stats.as_array().expect("reports");
    let first = reports.iter().find(|s| s["id"] == "video").expect("first session");
    let second = reports.iter().find(|s| s["id"] == "other-video").expect("second session");
    assert_eq!(first["powerEfficientDecoder"], true);
    assert_eq!(second["powerEfficientDecoder"], false);
}

#[test]
fn decoder_acceleration_corrects_only_unmeasured_videotoolbox_defaults() {
    for (scenario, expected) in [(6, None), (7, Some(false)), (8, Some(true))] {
        let json = ffi::decoder_acceleration_stats_for_test(scenario).expect("native report");
        let stats: serde_json::Value = serde_json::from_str(&json).expect("valid statistics");
        assert_eq!(stats[0]["powerEfficientDecoder"].as_bool(), expected);
    }
}

#[cfg(target_os = "macos")]
#[test]
#[ignore = "requires a real macOS VideoToolbox H.264 hardware decoder"]
fn decoder_acceleration_tracks_real_h264_session_replacement_and_release() {
    let snapshots = ffi::apple_h264_acceleration_for_test(
        include_bytes!("../tests/fixtures/h264-64x64.h264"),
        include_bytes!("../tests/fixtures/h264-96x64.h264"),
    )
    .expect("native H264 frames and measured hardware acceleration");
    let measured: Vec<_> = snapshots
        .iter()
        .map(|json| {
            let stats: serde_json::Value = serde_json::from_str(json).expect("valid statistics");
            assert_eq!(stats[0]["decoderImplementation"], "VideoToolbox");
            stats[0]["powerEfficientDecoder"].as_bool()
        })
        .collect();
    assert_eq!(measured, [None, Some(true), Some(true), None]);
}
