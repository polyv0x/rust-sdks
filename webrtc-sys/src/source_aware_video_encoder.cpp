/*
 * Copyright 2026 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/source_aware_video_encoder.h"

#include <utility>

#include "api/video/video_frame.h"
#include "livekit/encoded_video_frame_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

namespace livekit_ffi {

SourceAwareVideoEncoder::SourceAwareVideoEncoder(
    std::unique_ptr<VideoEncoder> raw_encoder,
    std::unique_ptr<VideoEncoder> encoded_encoder)
    : raw_encoder_(std::move(raw_encoder)),
      encoded_encoder_(std::move(encoded_encoder)) {}

int SourceAwareVideoEncoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                                       const Settings& settings) {
  if (!codec_settings) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (active_encoder_) {
    active_encoder_->Release();
    active_encoder_ = nullptr;
  }
  codec_ = *codec_settings;
  settings_ = settings;
  return WEBRTC_VIDEO_CODEC_OK;
}

int SourceAwareVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  return active_encoder_
             ? active_encoder_->RegisterEncodeCompleteCallback(callback)
             : WEBRTC_VIDEO_CODEC_OK;
}

int SourceAwareVideoEncoder::Release() {
  const int result = active_encoder_ ? active_encoder_->Release()
                                     : WEBRTC_VIDEO_CODEC_OK;
  active_encoder_ = nullptr;
  codec_.reset();
  settings_.reset();
  callback_ = nullptr;
  fec_controller_ = nullptr;
  rates_.reset();
  packet_loss_rate_.reset();
  rtt_ms_.reset();
  loss_notification_.reset();
  return result;
}

int SourceAwareVideoEncoder::Activate(bool encoded) {
  if (!codec_ || !settings_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  VideoEncoder* encoder = encoded ? encoded_encoder_.get() : raw_encoder_.get();
  if (!encoder) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  encoder->SetFecControllerOverride(fec_controller_);
  int result = encoder->InitEncode(&*codec_, *settings_);
  if (result == WEBRTC_VIDEO_CODEC_OK) {
    result = encoder->RegisterEncodeCompleteCallback(callback_);
  }
  if (result != WEBRTC_VIDEO_CODEC_OK) {
    encoder->Release();
    // A lazy pixel-encoder initialization failure must still enter WebRTC's
    // encoder-failure/selector path, just like an eager factory failure.
    return encoded ? result : WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  active_encoder_ = encoder;
  if (rates_) encoder->SetRates(*rates_);
  if (packet_loss_rate_) encoder->OnPacketLossRateUpdate(*packet_loss_rate_);
  if (rtt_ms_) encoder->OnRttUpdate(*rtt_ms_);
  if (loss_notification_) encoder->OnLossNotification(*loss_notification_);
  return WEBRTC_VIDEO_CODEC_OK;
}

int SourceAwareVideoEncoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  const auto buffer = frame.video_frame_buffer();
  const bool encoded =
      livekit::EncodedVideoFrameBuffer::FromNative(buffer.get()) != nullptr;
  if (!active_encoder_) {
    const int result = Activate(encoded);
    if (result != WEBRTC_VIDEO_CODEC_OK) return result;
  }
  if (encoded != (active_encoder_ == encoded_encoder_.get())) {
    RTC_LOG(LS_ERROR) << "Video source changed between encoded and pixel input "
                        "without reinitializing the encoder";
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  // Native support keeps our typed encoded buffers intact up to this check.
  // For genuine pixel buffers, preserve WebRTC's normal software conversion.
  if (!encoded && buffer->type() == webrtc::VideoFrameBuffer::Type::kNative &&
      !active_encoder_->GetEncoderInfo().supports_native_handle) {
    auto i420 = buffer->ToI420();
    if (!i420) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    webrtc::VideoFrame converted = frame;
    converted.set_video_frame_buffer(std::move(i420));
    return active_encoder_->Encode(converted, frame_types);
  }
  return active_encoder_->Encode(frame, frame_types);
}

void SourceAwareVideoEncoder::SetRates(const RateControlParameters& parameters) {
  rates_ = parameters;
  if (active_encoder_) active_encoder_->SetRates(parameters);
}

void SourceAwareVideoEncoder::SetFecControllerOverride(
    webrtc::FecControllerOverride* controller) {
  fec_controller_ = controller;
  if (active_encoder_) active_encoder_->SetFecControllerOverride(controller);
}

void SourceAwareVideoEncoder::OnPacketLossRateUpdate(float packet_loss_rate) {
  packet_loss_rate_ = packet_loss_rate;
  if (active_encoder_) active_encoder_->OnPacketLossRateUpdate(packet_loss_rate);
}

void SourceAwareVideoEncoder::OnRttUpdate(int64_t rtt_ms) {
  rtt_ms_ = rtt_ms;
  if (active_encoder_) active_encoder_->OnRttUpdate(rtt_ms);
}

void SourceAwareVideoEncoder::OnLossNotification(
    const LossNotification& notification) {
  loss_notification_ = notification;
  if (active_encoder_) active_encoder_->OnLossNotification(notification);
}

webrtc::VideoEncoder::EncoderInfo SourceAwareVideoEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  if (active_encoder_) {
    info = active_encoder_->GetEncoderInfo();
  } else {
    // No pixel encoder may impose CPU mapping or resolution adaptation on an
    // encoded source while the asynchronous backend selector is still pending.
    info.implementation_name = "LiveKit pending source";
    info.scaling_settings = ScalingSettings::kOff;
    info.supports_simulcast = true;
  }
  info.supports_native_handle = true;
  // Mapping a native buffer before Encode would erase its input-kind identity.
  info.preferred_pixel_formats = {webrtc::VideoFrameBuffer::Type::kNative};
  return info;
}

}  // namespace livekit_ffi
