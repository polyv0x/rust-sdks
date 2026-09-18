// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "livekit/external_video_decoder.h"
namespace livekit_ffi {
rust::Box<ExternalFrameOwner> external_decoder_roundtrip(rust::Box<ExternalFrameOwner> frame);
void external_decoder_revoked(rust::Box<ExternalFrameOwner> frame);
}
