// Copyright 2026 LiveKit, Inc. Licensed under the Apache License, Version 2.0.
#include "livekit/external_video_decoder.h"
#include "webrtc-sys/src/external_video_decoder.rs.h"
#include <unordered_map>
#include <utility>
#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace livekit_ffi {
namespace {
// RTTI is disabled in Chromium. This registry recognizes only our own native buffers;
// the caller's ref-counted VideoFrameBuffer keeps its owner alive while it is cloned.
std::mutex owners_mutex;
std::unordered_map<const webrtc::VideoFrameBuffer*, const ExternalFrameOwner*> owners;

class ExternalBuffer : public webrtc::VideoFrameBuffer {
 public:
  explicit ExternalBuffer(rust::Box<ExternalFrameOwner> owner) : owner_(std::move(owner)) {
    std::lock_guard lock(owners_mutex);
    owners.emplace(this, &*owner_);
  }
  ~ExternalBuffer() override {
    std::lock_guard lock(owners_mutex);
    owners.erase(this);
  }
  Type type() const override { return Type::kNative; }
  int width() const override { return owner_->frame_width(); }
  int height() const override { return owner_->frame_height(); }
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    auto pixels = owner_->frame_pixels();
    size_t y = size_t(width()) * height();
    size_t chroma = size_t((width() + 1) / 2) * ((height() + 1) / 2);
    if (pixels.size() != y + 2 * chroma) return nullptr;
    return webrtc::I420Buffer::Copy(width(), height(), pixels.data(), width(),
        pixels.data() + y, (width() + 1) / 2, pixels.data() + y + chroma, (width() + 1) / 2);
  }
 private:
  rust::Box<ExternalFrameOwner> owner_;
};

class ExternalDecoder : public webrtc::VideoDecoder {
 public:
  explicit ExternalDecoder(rust::Box<ExternalDecoderHandle> handle) : handle_(std::move(handle)), output_(std::make_shared<ExternalDecoderCallback>()) {}
  ~ExternalDecoder() override { Release(); }
  bool Configure(const Settings& settings) override {
    output_->Revoke();
    handle_->release_decoder();
    output_ = std::make_shared<ExternalDecoderCallback>();
    output_->Set(callback_);
    return handle_->configure_decoder({settings.max_render_resolution().Width(), settings.max_render_resolution().Height(), settings.number_of_cores()}, output_);
  }
  int32_t Decode(const webrtc::EncodedImage& image, int64_t render_time_ms) override {
    if (image.size() == 0 || image.size() > 8 * 1024 * 1024) return WEBRTC_VIDEO_CODEC_ERROR;
    ExternalEncodedInput input;
    input.data.reserve(image.size());
    std::copy(image.data(), image.data() + image.size(), std::back_inserter(input.data));
    input.rtp_timestamp = image.RtpTimestamp();
    input.keyframe = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    input.render_time_ms = render_time_ms;
    switch (handle_->decode_input(std::move(input))) {
      case ExternalDecodeStatus::Accepted: return WEBRTC_VIDEO_CODEC_OK;
      case ExternalDecodeStatus::NeedKeyframe: return WEBRTC_VIDEO_CODEC_ERROR;
      default: return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
  }
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
    callback_ = callback;
    output_->Set(callback);
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Release() override {
    output_->Revoke();
    handle_->release_decoder();
    return WEBRTC_VIDEO_CODEC_OK;
  }
  DecoderInfo GetDecoderInfo() const override {
    auto info = handle_->decoder_info();
    return {std::string(info.name), info.hardware};
  }
 private:
  rust::Box<ExternalDecoderHandle> handle_;
  std::shared_ptr<ExternalDecoderCallback> output_;
  webrtc::DecodedImageCallback* callback_ = nullptr;
};
}

void ExternalDecoderCallback::Set(webrtc::DecodedImageCallback* callback) {
  std::lock_guard lock(mutex_);
  if (!revoked_) callback_ = callback;
}
void ExternalDecoderCallback::Revoke() {
  std::lock_guard lock(mutex_);
  revoked_ = true;
  callback_ = nullptr;
}
void ExternalDecoderCallback::decoded(rust::Box<ExternalFrameOwner> owner, uint32_t timestamp) const {
  std::lock_guard lock(mutex_);
  if (revoked_ || !callback_) return;
  auto buffer = webrtc::make_ref_counted<ExternalBuffer>(std::move(owner));
  auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(buffer).set_timestamp_rtp(timestamp).build();
  callback_->Decoded(frame, std::nullopt, std::nullopt);
}
rust::Box<ExternalFrameOwner> external_frame_owner(const VideoFrameBuffer& buffer) {
  std::lock_guard lock(owners_mutex);
  auto found = owners.find(buffer.get().get());
  return found == owners.end() ? empty_external_frame_owner() : found->second->clone_owner();
}
std::unique_ptr<webrtc::VideoDecoder> CreateExternalVideoDecoder(const std::string& codec) {
  auto decoder = create_external_decoder(codec);
  if (!decoder->decoder_available()) return nullptr;
  return std::make_unique<ExternalDecoder>(std::move(decoder));
}
}
