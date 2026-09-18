// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
use crate::external_video_decoder::{ExternalFrameOwner, NativeDecodedFrame};
use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Arc,
};

use crate::external_video_decoder::ffi;

struct TestFrame {
    copies: Arc<AtomicUsize>,
}
impl NativeDecodedFrame for TestFrame {
    fn dimensions(&self) -> (u32, u32) {
        (1920, 1080)
    }
    fn to_i420(&self) -> Option<Vec<u8>> {
        self.copies.fetch_add(1, Ordering::Relaxed);
        None
    }
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
}

#[test]
fn decoded_callback_preserves_native_owner_without_conversion() {
    let copies = Arc::new(AtomicUsize::new(0));
    let frame: Arc<dyn NativeDecodedFrame> = Arc::new(TestFrame { copies: copies.clone() });
    let weak = Arc::downgrade(&frame);
    let returned = ffi::external_decoder_roundtrip(Box::new(ExternalFrameOwner::from_frame(frame)));
    assert_eq!(copies.load(Ordering::Relaxed), 0);
    assert_eq!(returned.frame().expect("native owner").dimensions(), (1920, 1080));
    assert!(weak.upgrade().is_some());
    drop(returned);
    assert!(weak.upgrade().is_none());
}

#[test]
fn late_callback_after_release_drops_its_owner() {
    let frame: Arc<dyn NativeDecodedFrame> =
        Arc::new(TestFrame { copies: Arc::new(AtomicUsize::new(0)) });
    let weak = Arc::downgrade(&frame);
    ffi::external_decoder_revoked(Box::new(ExternalFrameOwner::from_frame(frame)));
    assert!(weak.upgrade().is_none());
}
