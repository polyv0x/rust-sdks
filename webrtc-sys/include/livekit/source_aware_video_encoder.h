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

#pragma once

#include <memory>
#include <optional>

#include "api/video_codecs/video_encoder.h"

namespace livekit_ffi {

// Selects a pixel encoder or passthrough from the first buffer, before either
// backend sees it. EncoderSelectorInterface is asynchronous and cannot enforce
// this input-type boundary: on Apple, passing our kNative encoded buffer to an
// ObjC encoder results in an invalid ObjCFrameBuffer cast and objc_retain crash.
class SourceAwareVideoEncoder final : public webrtc::VideoEncoder {
 public:
  // Owns separate pixel and encoded backends; neither is initialized eagerly.
  SourceAwareVideoEncoder(std::unique_ptr<VideoEncoder> raw_encoder,
                          std::unique_ptr<VideoEncoder> encoded_encoder);

  // Saves configuration until the first frame identifies the input kind.
  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const Settings& settings) override;
  // Keeps the callback available across lazy initialization and reconfiguration.
  int RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;
  // Releases the active backend and forgets all per-session state.
  int Release() override;
  // Dispatches before any native-buffer cast, conversion, or simulcast scaling.
  int Encode(const webrtc::VideoFrame& frame,
             const std::vector<webrtc::VideoFrameType>* frame_types) override;
  // Retains the latest target, including updates before the first frame.
  void SetRates(const RateControlParameters& parameters) override;
  // Forwards FEC configuration when a backend is initialized.
  void SetFecControllerOverride(
      webrtc::FecControllerOverride* controller) override;
  // Retains the latest packet-loss estimate for lazy initialization.
  void OnPacketLossRateUpdate(float packet_loss_rate) override;
  // Retains the latest round-trip estimate for lazy initialization.
  void OnRttUpdate(int64_t rtt_ms) override;
  // Retains the latest loss notification for lazy initialization.
  void OnLossNotification(const LossNotification& notification) override;
  // Advertises native input support so encoded buffers reach the type boundary.
  EncoderInfo GetEncoderInfo() const override;

 private:
  // Initializes exactly one backend and replays pending encoder controls.
  int Activate(bool encoded);

  std::unique_ptr<VideoEncoder> raw_encoder_;
  std::unique_ptr<VideoEncoder> encoded_encoder_;
  VideoEncoder* active_encoder_ = nullptr;
  std::optional<webrtc::VideoCodec> codec_;
  std::optional<Settings> settings_;
  webrtc::EncodedImageCallback* callback_ = nullptr;
  webrtc::FecControllerOverride* fec_controller_ = nullptr;
  std::optional<RateControlParameters> rates_;
  std::optional<float> packet_loss_rate_;
  std::optional<int64_t> rtt_ms_;
  std::optional<LossNotification> loss_notification_;
};

}  // namespace livekit_ffi
