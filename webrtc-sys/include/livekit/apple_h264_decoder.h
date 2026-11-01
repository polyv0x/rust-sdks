// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
#pragma once

#import "sdk/objc/base/RTCVideoDecoder.h"

namespace livekit_ffi {
// The WebRTC H.264 decoder with session-scoped VideoToolbox measurements.
id<RTC_OBJC_TYPE(RTCVideoDecoder)> CreateAppleH264Decoder();
}  // namespace livekit_ffi
