// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "rust/cxx.h"

namespace livekit_ffi {
rust::String decoder_acceleration_stats_for_test(uint8_t scenario);
#if defined(WEBRTC_MAC) && !defined(WEBRTC_IOS)
rust::Vec<rust::String> apple_h264_acceleration_for_test(
    rust::Slice<const uint8_t> first, rust::Slice<const uint8_t> replacement);
#endif
}  // namespace livekit_ffi
