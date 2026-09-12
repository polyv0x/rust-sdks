// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0

#include "livekit/encoder_tests.h"

#include <stdexcept>
#include <string>

#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "livekit/encoded_video_frame_buffer.h"
#include "livekit/passthrough_video_encoder.h"
#include "livekit/source_aware_video_encoder.h"
#include "livekit/video_encoder_factory.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

namespace livekit_ffi {
namespace {

using namespace webrtc;

// Asserts in release builds too, translating failures through the CXX bridge.
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Records synchronous passthrough output without decoding arbitrary test bytes.
class Output final : public EncodedImageCallback {
 public:
  // Copies callback facts while their borrowed storage is alive.
  Result OnEncodedImage(const EncodedImage& image,
                        const CodecSpecificInfo* codec) override {
    ++count;
    bytes.assign(image.data(), image.data() + image.size());
    timestamp = image.RtpTimestamp();
    codec_type = codec->codecType;
    return Result(Result::OK);
  }
  int count = 0;
  uint32_t timestamp = 0;
  VideoCodecType codec_type = kVideoCodecGeneric;
  std::vector<uint8_t> bytes;
};

// Models a software encoder that cannot consume native buffers.
class PixelEncoder final : public VideoEncoder {
 public:
  // Tracks initialization without opening any hardware encoder.
  int InitEncode(const VideoCodec*, const Settings&) override {
    ++initializations;
    return fail_initialization ? WEBRTC_VIDEO_CODEC_ERROR : WEBRTC_VIDEO_CODEC_OK;
  }
  // Accepts the callback so initialization order remains observable.
  int RegisterEncodeCompleteCallback(EncodedImageCallback*) override { return 0; }
  // Records cleanup, including failed initializations.
  int Release() override { ++releases; return 0; }
  // Rejects all native input to catch missing software conversion.
  int Encode(const VideoFrame& frame,
             const std::vector<VideoFrameType>*) override {
    Require(frame.video_frame_buffer()->type() == VideoFrameBuffer::Type::kI420,
            "pixel encoder received a native/encoded buffer");
    ++frames;
    return 0;
  }
  // Records replay of controls submitted before the first frame.
  void SetRates(const RateControlParameters& rates) override {
    bitrate = rates.bitrate.get_sum_bps();
  }
  // Records packet-loss replay.
  void OnPacketLossRateUpdate(float rate) override { loss = rate; }
  // Records round-trip replay.
  void OnRttUpdate(int64_t value) override { rtt = value; }
  // Advertises a normal software encoder's input requirements.
  EncoderInfo GetEncoderInfo() const override {
    EncoderInfo info;
    info.implementation_name = "test pixel encoder";
    info.supports_native_handle = false;
    return info;
  }
  int initializations = 0;
  int releases = 0;
  int frames = 0;
  uint32_t bitrate = 0;
  float loss = 0;
  int64_t rtt = 0;
  bool fail_initialization = false;
};

// Models a native camera buffer whose pixels can be mapped to I420.
class NativePixels : public VideoFrameBuffer {
 public:
  // Marks this as native without pretending to be an encoded access unit.
  Type type() const override { return Type::kNative; }
  // Returns the fixture width.
  int width() const override { return 128; }
  // Returns the fixture height.
  int height() const override { return 128; }
  // Supplies CPU pixels to a software encoder.
  scoped_refptr<I420BufferInterface> ToI420() override {
    auto pixels = I420Buffer::Create(width(), height());
    I420Buffer::SetBlack(pixels.get());
    return pixels;
  }
};

// Builds the constrained-baseline format used by native macOS publication.
SdpVideoFormat H264Format() {
  return SdpVideoFormat("H264", {{"profile-level-id", "42e01f"},
                               {"level-asymmetry-allowed", "1"},
                               {"packetization-mode", "1"}});
}

// Configures a single-layer stream, including a rate update before any frame.
void Initialize(VideoEncoder& encoder, VideoCodecType codec_type,
                Output& output) {
  VideoCodec codec;
  codec.codecType = codec_type;
  codec.width = 128;
  codec.height = 128;
  codec.maxFramerate = 30;
  codec.startBitrate = 1000;
  codec.maxBitrate = 2000;
  codec.minBitrate = 100;
  codec.mode = VideoCodecMode::kScreensharing;
  codec.numberOfSimulcastStreams = 0;
  if (codec_type == kVideoCodecH264) {
    *codec.H264() = VideoEncoder::GetDefaultH264Settings();
  }
  Require(encoder.InitEncode(&codec, VideoEncoder::Settings(
      VideoEncoder::Capabilities(false), 1, 1200)) == 0, "InitEncode failed");
  Require(encoder.RegisterEncodeCompleteCallback(&output) == 0,
          "callback registration failed");
  VideoBitrateAllocation bitrate;
  bitrate.SetBitrate(0, 0, 1000000);
  encoder.SetRates(VideoEncoder::RateControlParameters(bitrate, 30));
}

// Builds the exact native-buffer type involved in the macOS crash.
VideoFrame Encoded(livekit::EncodedVideoCodec codec,
                   livekit::EncodedFrameType type,
                   const std::vector<uint8_t>& bytes,
                   std::shared_ptr<std::atomic<bool>> keyframe = nullptr,
                   std::shared_ptr<livekit::EncodedRateControlState> rates = nullptr) {
  return VideoFrame::Builder()
      .set_video_frame_buffer(make_ref_counted<livekit::EncodedVideoFrameBuffer>(
          128, 128, codec, type,
          EncodedImageBuffer::Create(bytes.data(), bytes.size()), keyframe, rates))
      .set_rtp_timestamp(9000)
      .set_timestamp_us(100000)
      .build();
}

// Exercises the real, untagged factory before any encoder selector feedback.
void FactoryFirstFrame(bool av1) {
  VideoEncoderFactory factory;
  auto encoder = factory.Create(CreateEnvironment(),
                                av1 ? SdpVideoFormat::AV1Profile0() : H264Format());
  Require(encoder != nullptr, "production factory did not create encoder");
  Require(encoder->GetEncoderInfo().supports_native_handle,
          "initial encoder info would map away encoded input");
  Output output;
  Initialize(*encoder, av1 ? kVideoCodecAV1 : kVideoCodecH264, output);
  auto request = std::make_shared<std::atomic<bool>>(false);
  auto rates = std::make_shared<livekit::EncodedRateControlState>();
  // Synthetic gray 128x128 L1T1 AV1 generated with SVT-AV1 4.2.0, no user content.
  const std::vector<uint8_t> bytes = av1
      ? std::vector<uint8_t>{0x12, 0, 0x0a, 0x0b, 2, 0, 0, 4, 0x19, 0xbf, 0xff,
                            0x12, 0xbe, 0x40, 0x10, 0x32, 0x0f, 0x10, 0, 0x85,
                            0xa0, 4, 0x10, 0x20, 0x80, 0, 0, 0, 0xec, 0x89, 0x5c, 0xa8}
      : std::vector<uint8_t>{0, 0, 0, 1, 0x65, 0x88, 0x84};
  auto frame = Encoded(av1 ? livekit::EncodedVideoCodec::kAV1
                           : livekit::EncodedVideoCodec::kH264,
                       livekit::EncodedFrameType::kKey, bytes, request, rates);
  const std::vector<VideoFrameType> key{VideoFrameType::kVideoFrameKey};
  Require(encoder->Encode(frame, &key) == 0, "first encoded frame failed");
  Require(output.count == 1 && output.timestamp == 9000,
          "first frame was dropped or not passed through synchronously");
  Require(output.codec_type == (av1 ? kVideoCodecAV1 : kVideoCodecH264),
          "passthrough codec mismatch");
  Require(encoder->GetEncoderInfo().implementation_name ==
              "LiveKit pre-encoded passthrough", "pixel encoder was selected");
  const auto target = rates->Take();
  Require(target.has_request && target.target_bitrate_bps == 1000000 &&
              target.framerate_fps == 30, "initial bitrate feedback was lost");
  if (!av1) {
    Require(output.bytes == bytes, "H.264 bytes were re-encoded");
    auto delta = Encoded(livekit::EncodedVideoCodec::kH264,
                         livekit::EncodedFrameType::kDelta, bytes, request);
    Require(encoder->Encode(delta, &key) == 0 && request->load(),
            "keyframe request did not reach the source");
  }
  Require(encoder->Release() == 0 && encoder->Release() == 0,
          "release was not idempotent");
}

// Covers raw conversion, type isolation, reinitialization, and failure handling.
void PixelScenario(uint8_t scenario) {
  auto raw = std::make_unique<PixelEncoder>();
  auto* probe = raw.get();
  probe->fail_initialization = scenario == 5;
  SourceAwareVideoEncoder encoder(
      std::move(raw), PassthroughVideoEncoderFactory().Create(
                          CreateEnvironment(), H264Format()));
  Output output;
  Initialize(encoder, kVideoCodecH264, output);
  encoder.OnPacketLossRateUpdate(0.25f);
  encoder.OnRttUpdate(42);
  Require(probe->initializations == 0, "pixel encoder initialized speculatively");
  scoped_refptr<VideoFrameBuffer> buffer = scenario == 3
      ? scoped_refptr<VideoFrameBuffer>(make_ref_counted<NativePixels>())
      : scoped_refptr<VideoFrameBuffer>(I420Buffer::Create(128, 128));
  auto frame = VideoFrame::Builder().set_video_frame_buffer(buffer).build();
  int result = encoder.Encode(frame, nullptr);
  if (scenario == 5) {
    Require(result == WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE &&
                probe->frames == 0 && probe->releases == 1,
            "lazy initialization failure did not request recovery");
    return;
  }
  Require(result == 0 && probe->frames == 1 && probe->initializations == 1,
          "raw frame was not encoded exactly once");
  Require(probe->bitrate == 1000000 && probe->loss == 0.25f && probe->rtt == 42,
          "pending raw encoder controls were not replayed");
  if (scenario == 4) {
    auto encoded = Encoded(livekit::EncodedVideoCodec::kH264,
                           livekit::EncodedFrameType::kKey, {0, 0, 1, 0x65, 0x88});
    Require(encoder.Encode(encoded, nullptr) == WEBRTC_VIDEO_CODEC_ERR_PARAMETER &&
                probe->frames == 1, "mixed encoded input reached pixel encoder");
    Initialize(encoder, kVideoCodecH264, output);
    Require(encoder.Encode(encoded, nullptr) == 0 && output.count == 1,
            "reinitialized encoder did not adopt the new input kind");
    Require(encoder.Encode(frame, nullptr) == WEBRTC_VIDEO_CODEC_ERR_PARAMETER,
            "passthrough accepted a raw pixel frame");
  }
  encoder.Release();
}

}  // namespace

void run_encoder_test(uint8_t scenario) {
  if (scenario <= 1) {
    FactoryFirstFrame(scenario == 1);
  } else {
    Require(scenario <= 5, "unknown encoder test");
    PixelScenario(scenario);
  }
}

}  // namespace livekit_ffi
