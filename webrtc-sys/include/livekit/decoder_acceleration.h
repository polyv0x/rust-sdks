// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "api/stats/rtc_stats_report.h"

namespace livekit_ffi {

// A decoder instance owns its measurement. Its internal name associates cached
// WebRTC reports with that instance, including after a decoder is replaced.
class VideoToolboxDecoderAcceleration {
 public:
  VideoToolboxDecoderAcceleration();
  ~VideoToolboxDecoderAcceleration();
  VideoToolboxDecoderAcceleration(const VideoToolboxDecoderAcceleration&) = delete;
  VideoToolboxDecoderAcceleration& operator=(const VideoToolboxDecoderAcceleration&) = delete;

  const std::string& implementation_name() const { return implementation_name_; }
  void Set(std::optional<bool> hardware);
  std::optional<bool> Get() const;

 private:
  const std::string implementation_name_;
  const std::shared_ptr<std::atomic<int>> hardware_;
};

// Preserve absent acceleration measurements and remove internal instance names
// at the SDK statistics boundary. The immutable WebRTC report is not modified.
std::string SerializeRtcStats(const webrtc::RTCStatsReport& report);

}  // namespace livekit_ffi
