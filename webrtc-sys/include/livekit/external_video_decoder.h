// Copyright 2026 LiveKit, Inc. Licensed under the Apache License, Version 2.0.
#pragma once
#include <memory>
#include <mutex>
#include "rust/cxx.h"
#include "api/video_codecs/video_decoder.h"
#include "livekit/video_frame_buffer.h"

namespace livekit_ffi {
struct ExternalFrameOwner;
class ExternalDecoderCallback {
 public:
  void decoded(rust::Box<ExternalFrameOwner> frame, uint32_t timestamp) const;
  void Set(webrtc::DecodedImageCallback* callback);
  void Revoke();
 private:
  mutable std::mutex mutex_;
  webrtc::DecodedImageCallback* callback_ = nullptr;
  bool revoked_ = false;
};
rust::Box<ExternalFrameOwner> external_frame_owner(const VideoFrameBuffer& buffer);
std::unique_ptr<webrtc::VideoDecoder> CreateExternalVideoDecoder(const std::string& codec);
}
