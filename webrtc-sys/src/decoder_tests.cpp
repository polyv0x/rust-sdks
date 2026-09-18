// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
#include "livekit/decoder_tests.h"
#include "webrtc-sys/src/external_video_decoder.rs.h"
#include <cassert>

namespace livekit_ffi {
namespace {
class Callback : public webrtc::DecodedImageCallback {
 public:
  rust::Box<ExternalFrameOwner> owner = empty_external_frame_owner();
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    assert(frame.rtp_timestamp() == 123456);
    assert(frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative);
    VideoFrameBuffer wrapper(frame.video_frame_buffer());
    owner = external_frame_owner(wrapper);
    return 0;
  }
};
}
rust::Box<ExternalFrameOwner> external_decoder_roundtrip(rust::Box<ExternalFrameOwner> frame) {
  Callback callback;
  ExternalDecoderCallback output;
  output.Set(&callback);
  output.decoded(std::move(frame), 123456);
  output.Revoke();
  return std::move(callback.owner);
}
void external_decoder_revoked(rust::Box<ExternalFrameOwner> frame) {
  ExternalDecoderCallback output;
  output.Revoke();
  output.decoded(std::move(frame), 123456);
}
}
