// Copyright 2026 LiveKit, Inc. SPDX-License-Identifier: Apache-2.0
#include "livekit/decoder_acceleration.h"

#include <cstdint>
#include <map>
#include <mutex>

#include "api/stats/rtcstats_objects.h"

namespace livekit_ffi {
namespace {
constexpr char kPrefix[] = "VideoToolbox/session/";

struct Measurements {
  std::mutex mutex;
  uint64_t next_id = 0;
  std::map<std::string, std::weak_ptr<std::atomic<int>>> values;
};

Measurements& Registry() {
  // Decoders can outlive other process statics during shutdown. Entries are
  // removed by their owners; only the registry itself has process lifetime.
  static auto* registry = new Measurements;
  return *registry;
}

std::optional<bool> Measurement(int value) {
  if (value < 0) return std::nullopt;
  return value != 0;
}

std::string NextName() {
  auto& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  return kPrefix + std::to_string(++registry.next_id);
}

std::optional<bool> Lookup(const std::string& name) {
  auto& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto it = registry.values.find(name);
  if (it == registry.values.end()) return std::nullopt;
  auto value = it->second.lock();
  return value ? Measurement(value->load(std::memory_order_relaxed)) : std::nullopt;
}
}  // namespace

VideoToolboxDecoderAcceleration::VideoToolboxDecoderAcceleration()
    : implementation_name_(NextName()), hardware_(std::make_shared<std::atomic<int>>(-1)) {
  auto& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.values.emplace(implementation_name_, hardware_);
}

VideoToolboxDecoderAcceleration::~VideoToolboxDecoderAcceleration() {
  auto& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.values.erase(implementation_name_);
}

void VideoToolboxDecoderAcceleration::Set(std::optional<bool> hardware) {
  hardware_->store(hardware ? (*hardware ? 1 : 0) : -1, std::memory_order_relaxed);
}

std::optional<bool> VideoToolboxDecoderAcceleration::Get() const {
  return Measurement(hardware_->load(std::memory_order_relaxed));
}

std::string SerializeRtcStats(const webrtc::RTCStatsReport& report) {
  // Serializing each copied entry avoids retaining or modifying WebRTC's cached
  // report and preserves every unrelated statistic, including timestamps.
  auto normalized = webrtc::RTCStatsReport::Create(report.timestamp());
  for (const auto& entry : report) {
    auto copy = entry.copy();
    if (copy->type() == webrtc::RTCInboundRtpStreamStats::kType) {
      auto& inbound = static_cast<webrtc::RTCInboundRtpStreamStats&>(*copy);
      if (inbound.decoder_implementation) {
        const auto& name = *inbound.decoder_implementation;
        if (name.rfind(kPrefix, 0) == 0) {
          inbound.power_efficient_decoder = Lookup(name);
          inbound.decoder_implementation = "VideoToolbox";
        } else if (name == "VideoToolbox" &&
                   !inbound.power_efficient_decoder.value_or(false)) {
          // Uninstrumented Objective-C decoders inherit false from the native
          // base class. That value does not measure software decoding.
          inbound.power_efficient_decoder.reset();
        }
      }
    }
    normalized->AddStats(std::move(copy));
  }
  return normalized->ToJson();
}
}  // namespace livekit_ffi
