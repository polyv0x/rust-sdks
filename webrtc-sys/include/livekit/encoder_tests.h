// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace livekit_ffi {
// Runs native assertions for one test scenario, throwing on failure.
void run_encoder_test(uint8_t scenario);
}  // namespace livekit_ffi
