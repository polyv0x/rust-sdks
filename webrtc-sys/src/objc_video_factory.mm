/*
 * Copyright 2025 LiveKit, Inc.
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

#include "livekit/objc_video_factory.h"

#import <sdk/objc/components/video_codec/RTCDefaultVideoDecoderFactory.h>
#import <sdk/objc/components/video_codec/RTCDefaultVideoEncoderFactory.h>
#import <sdk/objc/components/video_codec/RTCVideoEncoderFactorySimulcast.h>
#include "sdk/objc/native/api/video_decoder_factory.h"
#include "sdk/objc/native/api/video_encoder_factory.h"

#if defined(WEBRTC_MAC) && !defined(WEBRTC_IOS)
#include "livekit/apple_h264_decoder.h"

@interface LKMeasuredVideoDecoderFactory : RTC_OBJC_TYPE(RTCDefaultVideoDecoderFactory)
@end

@implementation LKMeasuredVideoDecoderFactory
- (id<RTC_OBJC_TYPE(RTCVideoDecoder)>)createDecoder:(RTC_OBJC_TYPE(RTCVideoCodecInfo)*)info {
  if ([info.name isEqualToString:@"H264"]) {
    return livekit_ffi::CreateAppleH264Decoder();
  }
  return [super createDecoder:info];
}
@end
#endif

namespace livekit_ffi {

std::unique_ptr<webrtc::VideoEncoderFactory> CreateObjCVideoEncoderFactory() {
  RTC_OBJC_TYPE(RTCDefaultVideoEncoderFactory)* encoderFactory = [[RTC_OBJC_TYPE(RTCDefaultVideoEncoderFactory) alloc] init];
  RTC_OBJC_TYPE(RTCVideoEncoderFactorySimulcast)* simulcastFactory =
      [[RTC_OBJC_TYPE(RTCVideoEncoderFactorySimulcast) alloc] initWithPrimary:encoderFactory fallback:encoderFactory];
  return webrtc::ObjCToNativeVideoEncoderFactory(simulcastFactory);
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateObjCVideoDecoderFactory() {
#if defined(WEBRTC_MAC) && !defined(WEBRTC_IOS)
  auto* factory = [[LKMeasuredVideoDecoderFactory alloc] init];
  auto native = webrtc::ObjCToNativeVideoDecoderFactory(factory);
  [factory release];  // The native adapter retains its Objective-C factory.
  return native;
#else
  return webrtc::ObjCToNativeVideoDecoderFactory([[RTC_OBJC_TYPE(RTCDefaultVideoDecoderFactory) alloc] init]);
#endif
}

}  // namespace livekit_ffi
