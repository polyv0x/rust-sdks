// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0

//! Deterministic tests at the native encoder factory boundary.

#[cxx::bridge(namespace = "livekit_ffi")]
mod ffi {
    // SAFETY: The harness accepts only a numeric test identifier and translates
    // assertion exceptions into Result; it does not expose native pointers.
    unsafe extern "C++" {
        include!("livekit/encoder_tests.h");

        /// Runs one native encoder regression scenario.
        fn run_encoder_test(scenario: u8) -> Result<()>;
    }
}

/// The untagged production factory must pass through the very first H.264 frame.
#[test]
fn untagged_h264_first_frame_and_feedback() {
    ffi::run_encoder_test(0).expect("H.264 first-frame passthrough");
}

/// AV1 input must reach passthrough without invoking a pixel encoder.
#[test]
fn untagged_av1_first_frame() {
    ffi::run_encoder_test(1).expect("AV1 first-frame passthrough");
}

/// Pixel input must initialize and use the original encoder and its controls.
#[test]
fn raw_pixels_and_controls_are_preserved() {
    ffi::run_encoder_test(2).expect("pixel encoder dispatch");
}

/// Native pixel input must retain software conversion without erasing encoded types.
#[test]
fn native_pixels_convert_for_software_encoder() {
    ffi::run_encoder_test(3).expect("native pixel conversion");
}

/// A source cannot switch buffer types inside an active encoder session.
#[test]
fn mixed_input_fails_closed_and_reinitialization_recovers() {
    ffi::run_encoder_test(4).expect("input type isolation");
}

/// A delayed pixel initialization failure must reach WebRTC's recovery path.
#[test]
fn raw_initialization_failure_requests_recovery() {
    ffi::run_encoder_test(5).expect("pixel encoder failure propagation");
}
