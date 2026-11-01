// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
#include "livekit/decoder_acceleration_tests.h"
#include "livekit/decoder_acceleration.h"
#include "api/stats/rtcstats_objects.h"

#include <stdexcept>

namespace livekit_ffi {
namespace {
auto Report(const std::string& name, bool hardware = false) {
  auto report = webrtc::RTCStatsReport::Create(webrtc::Timestamp::Millis(123));
  auto inbound = std::make_unique<webrtc::RTCInboundRtpStreamStats>(
      "video", report->timestamp());
  inbound->kind = "video";
  inbound->ssrc = 42;
  inbound->frames_decoded = 7;
  inbound->decoder_implementation = name;
  inbound->power_efficient_decoder = hardware;
  report->AddStats(std::move(inbound));
  return report;
}
}  // namespace

rust::String decoder_acceleration_stats_for_test(uint8_t scenario) {
  VideoToolboxDecoderAcceleration first;
  auto report = Report(first.implementation_name());
  switch (scenario) {
    case 0: break;  // No session or failed property query.
    case 1: first.Set(true); break;
    case 2: first.Set(false); break;
    case 3:
      first.Set(true);
      first.Set(std::nullopt);  // Session reset or release.
      break;
    case 4: {
      VideoToolboxDecoderAcceleration retired;
      retired.Set(true);
      report = Report(retired.implementation_name());
      break;  // The cached report outlives its decoder.
    }
    case 5: {
      first.Set(true);
      VideoToolboxDecoderAcceleration second;
      second.Set(false);
      auto inbound = std::make_unique<webrtc::RTCInboundRtpStreamStats>(
          "other-video", report->timestamp());
      inbound->decoder_implementation = second.implementation_name();
      inbound->power_efficient_decoder = true;  // Deliberately stale upstream value.
      report->AddStats(std::move(inbound));
      return SerializeRtcStats(*report);
    }
    case 6: report = Report("VideoToolbox"); break;
    case 7: report = Report("FFmpeg"); break;
    case 8: report = Report("VideoToolbox", true); break;
    default: throw std::runtime_error("unknown acceleration test");
  }
  const auto original = report->ToJson();
  auto normalized = SerializeRtcStats(*report);
  if (original != report->ToJson()) throw std::runtime_error("modified cached report");
  return normalized;
}
}  // namespace livekit_ffi

#if defined(WEBRTC_MAC) && !defined(WEBRTC_IOS)
#include "livekit/objc_video_factory.h"
#include "api/environment/environment_factory.h"
#include "api/video/encoded_image.h"
#include "modules/video_coding/include/video_error_codes.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace livekit_ffi {
namespace {
class NativeFrames : public webrtc::DecodedImageCallback {
 public:
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    native_ = frame.video_frame_buffer()->type() == webrtc::VideoFrameBuffer::Type::kNative;
    ++count_;
    ready_.notify_one();
    return WEBRTC_VIDEO_CODEC_OK;
  }
  void Wait(size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::seconds(5), [&] { return count_ >= count; })) {
      throw std::runtime_error("VideoToolbox did not deliver a decoded frame");
    }
    if (!native_) throw std::runtime_error("H264 output lost its native pixel buffer");
  }
 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  size_t count_ = 0;
  bool native_ = false;
};
}  // namespace

rust::Vec<rust::String> apple_h264_acceleration_for_test(
    rust::Slice<const uint8_t> first, rust::Slice<const uint8_t> replacement) {
  NativeFrames frames;
  auto factory = CreateObjCVideoDecoderFactory();
  auto decoder = factory->Create(webrtc::CreateEnvironment(), webrtc::SdpVideoFormat("H264"));
  webrtc::VideoDecoder::Settings settings;
  settings.set_codec_type(webrtc::kVideoCodecH264);
  if (!decoder || !decoder->Configure(settings)) throw std::runtime_error("H264 configuration failed");
  decoder->RegisterDecodeCompleteCallback(&frames);
  rust::Vec<rust::String> snapshots;
  auto snapshot = [&] {
    auto info = decoder->GetDecoderInfo();
    snapshots.push_back(SerializeRtcStats(*Report(info.implementation_name, info.is_hardware_accelerated)));
  };
  snapshot();
  uint32_t timestamp = 0;
  for (auto bytes : {first, replacement}) {
    webrtc::EncodedImage input;
    input.SetEncodedData(webrtc::EncodedImageBuffer::Create(bytes.data(), bytes.size()));
    input._frameType = webrtc::VideoFrameType::kVideoFrameKey;
    input.SetRtpTimestamp(++timestamp);
    if (decoder->Decode(input, 0) != WEBRTC_VIDEO_CODEC_OK) throw std::runtime_error("H264 decode failed");
    frames.Wait(timestamp);
    if (!decoder->GetDecoderInfo().is_hardware_accelerated) {
      throw std::runtime_error("VideoToolbox did not report a hardware decoder");
    }
    snapshot();
  }
  decoder->Release();
  snapshot();
  return snapshots;
}
}  // namespace livekit_ffi
#endif
