/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in apple_h264_decoder.LICENSE alongside this file.
 *  An additional intellectual property rights grant can be found in the file
 *  apple_h264_decoder.PATENTS. Contributing authors are listed in WebRTC's
 *  upstream AUTHORS file.
 *
 */

// Based on webrtc-sdk/webrtc 30d5e63ae91da483e06577b5c35ee91cc5e5c3db
// sdk/objc/components/video_codec/RTCVideoDecoderH264.mm (m144_release).
// This private class retains the upstream decode path and adds measured session
// acceleration. A separate class avoids overriding symbols in the prebuilt SDK.
#import "livekit/apple_h264_decoder.h"
#include "livekit/decoder_acceleration.h"
#import "sdk/objc/api/peerconnection/RTCEncodedImage+Private.h"
#import "sdk/objc/api/video_codec/RTCNativeVideoDecoderBuilder+Native.h"
#include "sdk/objc/native/src/objc_frame_buffer.h"

#import <VideoToolbox/VideoToolbox.h>

#import "base/RTCVideoFrame.h"
#import "base/RTCVideoFrameBuffer.h"
#import "components/video_frame_buffer/RTCCVPixelBuffer.h"
#import "sdk/objc/components/video_codec/helpers.h"
#import "helpers/scoped_cftyperef.h"

#if defined(WEBRTC_IOS)
#import "helpers/UIDevice+RTCDevice.h"
#endif

#include "api/array_view.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "sdk/objc/components/video_codec/nalu_rewriter.h"

// Struct that we pass to the decoder per frame to decode. We receive it again
// in the decoder callback.
struct LKFrameDecodeParams {
  LKFrameDecodeParams(RTCVideoDecoderCallback cb, int64_t ts)
      : callback(cb), timestamp(ts) {}
  RTCVideoDecoderCallback callback;
  int64_t timestamp;
};

@interface LKRTCVideoDecoderH264 : NSObject
    <RTC_OBJC_TYPE(RTCVideoDecoder), RTC_OBJC_TYPE(RTCNativeVideoDecoderBuilder)>
- (void)setError:(OSStatus)error;
- (livekit_ffi::VideoToolboxDecoderAcceleration&)acceleration;
@end

namespace livekit_ffi {
namespace {
class MeasuredH264Decoder : public webrtc::VideoDecoder {
 public:
  explicit MeasuredH264Decoder(LKRTCVideoDecoderH264* decoder) : decoder_(decoder) {}
  ~MeasuredH264Decoder() override { [decoder_ releaseDecoder]; }

  bool Configure(const Settings& settings) override {
    return [decoder_ startDecodeWithNumberOfCores:settings.number_of_cores()] ==
           WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Decode(const webrtc::EncodedImage& input, int64_t render_time_ms) override {
    auto* encoded = [[RTC_OBJC_TYPE(RTCEncodedImage) alloc] initWithNativeEncodedImage:input];
    return [decoder_ decode:encoded missingFrames:NO codecSpecificInfo:nil renderTimeMs:render_time_ms];
  }
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
    [decoder_ setCallback:^(RTC_OBJC_TYPE(RTCVideoFrame)* frame) {
      auto buffer = webrtc::make_ref_counted<webrtc::ObjCFrameBuffer>(frame.buffer);
      auto native = webrtc::VideoFrame::Builder()
                        .set_video_frame_buffer(buffer)
                        .set_rtp_timestamp(frame.timeStamp)
                        .set_timestamp_ms(0)
                        .set_rotation(static_cast<webrtc::VideoRotation>(frame.rotation))
                        .build();
      callback->Decoded(native);
    }];
    return WEBRTC_VIDEO_CODEC_OK;
  }
  int32_t Release() override { return [decoder_ releaseDecoder]; }
  DecoderInfo GetDecoderInfo() const override {
    auto& measurement = [decoder_ acceleration];
    return {measurement.implementation_name(), measurement.Get().value_or(false)};
  }
  const char* ImplementationName() const override {
    return [decoder_ acceleration].implementation_name().c_str();
  }

 private:
  LKRTCVideoDecoderH264* decoder_;
};
}  // namespace
}  // namespace livekit_ffi

// This is the callback function that VideoToolbox calls when decode is
// complete.
static void LKDecompressionOutputCallback(void *decoderRef,
                                 void *params,
                                 OSStatus status,
                                 VTDecodeInfoFlags /*infoFlags*/,
                                 CVImageBufferRef imageBuffer,
                                 CMTime timestamp,
                                 CMTime /*duration*/) {
  std::unique_ptr<LKFrameDecodeParams> decodeParams(
      reinterpret_cast<LKFrameDecodeParams *>(params));
  if (status != noErr) {
    LKRTCVideoDecoderH264 *decoder =
        (__bridge LKRTCVideoDecoderH264 *)decoderRef;
    [decoder setError:status];
    RTC_LOG(LS_ERROR) << "Failed to decode frame. Status: " << status;
    return;
  }
  // TODO(tkchin): Handle CVO properly.
  RTC_OBJC_TYPE(RTCCVPixelBuffer) *frameBuffer =
      [[RTC_OBJC_TYPE(RTCCVPixelBuffer) alloc] initWithPixelBuffer:imageBuffer];
  RTC_OBJC_TYPE(
      RTCVideoFrame) *decodedFrame = [[RTC_OBJC_TYPE(RTCVideoFrame) alloc]
      initWithBuffer:frameBuffer
            rotation:RTC_OBJC_TYPE(RTCVideoRotation_0)
         timeStampNs:CMTimeGetSeconds(timestamp) * webrtc::kNumNanosecsPerSec];
  decodedFrame.timeStamp = decodeParams->timestamp;
  decodeParams->callback(decodedFrame);
}

// Decoder.
@implementation LKRTCVideoDecoderH264 {
  CMVideoFormatDescriptionRef _videoFormat;
  CMMemoryPoolRef _memoryPool;
  VTDecompressionSessionRef _decompressionSession;
  RTCVideoDecoderCallback _callback;
  OSStatus _error;
  livekit_ffi::VideoToolboxDecoderAcceleration _acceleration;
}

- (std::unique_ptr<webrtc::VideoDecoder>)build:(const webrtc::Environment&)env {
  return std::make_unique<livekit_ffi::MeasuredH264Decoder>(self);
}

- (livekit_ffi::VideoToolboxDecoderAcceleration&)acceleration {
  return _acceleration;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _memoryPool = CMMemoryPoolCreate(nil);
  }
  return self;
}

- (void)dealloc {
  CMMemoryPoolInvalidate(_memoryPool);
  CFRelease(_memoryPool);
  [self destroyDecompressionSession];
  [self setVideoFormat:nullptr];
}

- (NSInteger)startDecodeWithNumberOfCores:(int)numberOfCores {
  return WEBRTC_VIDEO_CODEC_OK;
}

// TODO(bugs.webrtc.org/15444): Remove obsolete missingFrames param.
- (NSInteger)decode:(RTC_OBJC_TYPE(RTCEncodedImage) *)inputImage
        missingFrames:(BOOL)missingFrames
    codecSpecificInfo:(nullable id<RTC_OBJC_TYPE(RTCCodecSpecificInfo)>)info
         renderTimeMs:(int64_t)renderTimeMs {
  RTC_DCHECK(inputImage.buffer);

  if (_error != noErr) {
    RTC_LOG(LS_WARNING) << "Last frame decode failed.";
    _error = noErr;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  webrtc::ScopedCFTypeRef<CMVideoFormatDescriptionRef> inputFormat =
      webrtc::ScopedCF(
          webrtc::CreateVideoFormatDescription(webrtc::MakeArrayView(
              (uint8_t *)inputImage.buffer.bytes, inputImage.buffer.length)));
  if (inputFormat) {
    // Check if the video format has changed, and reinitialize decoder if
    // needed.
    if (!CMFormatDescriptionEqual(inputFormat.get(), _videoFormat)) {
      [self setVideoFormat:inputFormat.get()];
      int resetDecompressionSessionError = [self resetDecompressionSession];
      if (resetDecompressionSessionError != WEBRTC_VIDEO_CODEC_OK) {
        return resetDecompressionSessionError;
      }
    }
  }
  if (!_videoFormat) {
    // We received a frame but we don't have format information so we can't
    // decode it.
    // This can happen after backgrounding. We need to wait for the next
    // sps/pps before we can resume so we request a keyframe by returning an
    // error.
    RTC_LOG(LS_WARNING) << "Missing video format. Frame with sps/pps required.";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  CMSampleBufferRef sampleBuffer = nullptr;
  if (!webrtc::H264AnnexBBufferToCMSampleBuffer(
          webrtc::MakeArrayView((uint8_t *)inputImage.buffer.bytes,
                                inputImage.buffer.length),
          _videoFormat,
          &sampleBuffer,
          _memoryPool)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  RTC_DCHECK(sampleBuffer);
  VTDecodeFrameFlags decodeFlags =
      kVTDecodeFrame_EnableAsynchronousDecompression;
  std::unique_ptr<LKFrameDecodeParams> frameDecodeParams;
  frameDecodeParams.reset(
      new LKFrameDecodeParams(_callback, inputImage.timeStamp));
  OSStatus status =
      VTDecompressionSessionDecodeFrame(_decompressionSession,
                                        sampleBuffer,
                                        decodeFlags,
                                        frameDecodeParams.release(),
                                        nullptr);
#if defined(WEBRTC_IOS)
  // Re-initialize the decoder if we have an invalid session while the app is
  // active or decoder malfunctions and retry the decode request.
  if ((status == kVTInvalidSessionErr ||
       status == kVTVideoDecoderMalfunctionErr) &&
      [self resetDecompressionSession] == WEBRTC_VIDEO_CODEC_OK) {
    RTC_LOG(LS_INFO) << "Failed to decode frame with code: " << status
                     << " retrying decode after decompression session reset";
    frameDecodeParams.reset(
        new LKFrameDecodeParams(_callback, inputImage.timeStamp));
    status = VTDecompressionSessionDecodeFrame(_decompressionSession,
                                               sampleBuffer,
                                               decodeFlags,
                                               frameDecodeParams.release(),
                                               nullptr);
  }
#endif
  CFRelease(sampleBuffer);
  if (status != noErr) {
    RTC_LOG(LS_ERROR) << "Failed to decode frame with code: " << status;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

- (void)setCallback:(RTCVideoDecoderCallback)callback {
  _callback = callback;
}

- (void)setError:(OSStatus)error {
  _error = error;
}

- (NSInteger)releaseDecoder {
  // Need to invalidate the session so that callbacks no longer occur and it
  // is safe to null out the callback.
  [self destroyDecompressionSession];
  [self setVideoFormat:nullptr];
  _callback = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

#pragma mark - Private

- (int)resetDecompressionSession {
  [self destroyDecompressionSession];

  // Need to wait for the first SPS to initialize decoder.
  if (!_videoFormat) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  // Set keys for OpenGL and IOSurface compatibilty, which makes the encoder
  // create pixel buffers with GPU backed memory. The intent here is to pass
  // the pixel buffers directly so we avoid a texture upload later during
  // rendering. This currently is moot because we are converting back to an
  // I420 frame after decode, but eventually we will be able to plumb
  // CVPixelBuffers directly to the renderer.
  // TODO(tkchin): Maybe only set OpenGL/IOSurface keys if we know that that
  // we can pass CVPixelBuffers as native handles in decoder output.
  NSDictionary *attributes = @{
#if defined(WEBRTC_IOS) && (TARGET_OS_MACCATALYST || TARGET_OS_SIMULATOR)
    (NSString *)kCVPixelBufferMetalCompatibilityKey : @(YES),
#elif defined(WEBRTC_IOS) && !defined(TARGET_OS_VISION)
    (NSString *)kCVPixelBufferOpenGLESCompatibilityKey : @(YES),
#elif defined(WEBRTC_MAC) && !defined(WEBRTC_ARCH_ARM64)
    (NSString *)kCVPixelBufferOpenGLCompatibilityKey : @(YES),
#endif
#if !(TARGET_OS_SIMULATOR)
    (NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
#endif
    (NSString *)kCVPixelBufferPixelFormatTypeKey :
        @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
  };

  VTDecompressionOutputCallbackRecord record = {
      LKDecompressionOutputCallback,
      (__bridge void *)self,
  };
  NSDictionary* specification = @{
    (NSString*)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder : @YES,
  };
  OSStatus status =
      VTDecompressionSessionCreate(nullptr,
                                   _videoFormat,
                                   (__bridge CFDictionaryRef)specification,
                                   (__bridge CFDictionaryRef)attributes,
                                   &record,
                                   &_decompressionSession);
  if (status != noErr) {
    RTC_LOG(LS_ERROR) << "Failed to create decompression session: " << status;
    [self destroyDecompressionSession];
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  [self configureDecompressionSession];

  return WEBRTC_VIDEO_CODEC_OK;
}

- (void)configureDecompressionSession {
  RTC_DCHECK(_decompressionSession);
  VTSessionSetProperty(_decompressionSession, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
  CFTypeRef value = nullptr;
  OSStatus status = VTSessionCopyProperty(
      _decompressionSession,
      kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
      kCFAllocatorDefault, &value);
  std::optional<bool> hardware;
  if (status == noErr && value && CFGetTypeID(value) == CFBooleanGetTypeID()) {
    hardware = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
  }
  if (value) CFRelease(value);
  _acceleration.Set(hardware);
}

- (void)destroyDecompressionSession {
  _acceleration.Set(std::nullopt);
  if (_decompressionSession) {
    // Complete callbacks before the native wrapper releases its callback owner.
    VTDecompressionSessionWaitForAsynchronousFrames(_decompressionSession);
    VTDecompressionSessionInvalidate(_decompressionSession);
    CFRelease(_decompressionSession);
    _decompressionSession = nullptr;
  }
}

- (void)setVideoFormat:(CMVideoFormatDescriptionRef)videoFormat {
  if (_videoFormat == videoFormat) {
    return;
  }
  if (_videoFormat) {
    CFRelease(_videoFormat);
  }
  _videoFormat = videoFormat;
  if (_videoFormat) {
    CFRetain(_videoFormat);
  }
}

- (NSString *)implementationName {
  return [NSString stringWithUTF8String:_acceleration.implementation_name().c_str()];
}

@end

namespace livekit_ffi {
id<RTC_OBJC_TYPE(RTCVideoDecoder)> CreateAppleH264Decoder() {
  return [[LKRTCVideoDecoderH264 alloc] init];
}
}  // namespace livekit_ffi
