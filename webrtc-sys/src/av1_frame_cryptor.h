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

#ifndef WEBRTC_AV1_FRAME_CRYPTOR_H_
#define WEBRTC_AV1_FRAME_CRYPTOR_H_

#include <map>
#include <memory>
#include <string>

#include "api/crypto/frame_crypto_transformer.h"
#include "api/frame_transformer_interface.h"
#include "api/scoped_refptr.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/thread.h"

namespace livekit_ffi {

bool IsAv1Frame(const webrtc::TransformableFrameInterface& frame);

/// AV1 needs codec-aware selective encryption: encrypting the complete encoded
/// frame hides the OBU boundaries from WebRTC's packetizer and the frame-type
/// bits from the SFU. This transformer uses LiveKit's existing key provider and
/// DataPacketCryptor while carrying the IV, key index, and authentication tag
/// in a final AV1 OBU_METADATA.
class Av1FrameCryptorTransformer : public webrtc::FrameTransformerInterface {
 public:
  Av1FrameCryptorTransformer(
      webrtc::Thread* signaling_thread,
      std::string participant_id,
      webrtc::FrameCryptorTransformer::Algorithm algorithm,
      webrtc::scoped_refptr<webrtc::KeyProvider> key_provider);
  ~Av1FrameCryptorTransformer() override;

  void Transform(
      std::unique_ptr<webrtc::TransformableFrameInterface> frame) override;
  void RegisterTransformedFrameCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback)
      override;
  void UnregisterTransformedFrameCallback() override;
  void RegisterTransformedFrameSinkCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
      uint32_t ssrc) override;
  void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override;

  void SetEnabled(bool enabled);
  bool enabled() const;
  void SetKeyIndex(int index);
  int key_index() const;
  void RegisterObserver(
      webrtc::scoped_refptr<webrtc::FrameCryptorTransformerObserver> observer);
  void UnregisterObserver();

 private:
  void EncryptFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame);
  void DecryptFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame);
  webrtc::scoped_refptr<webrtc::TransformedFrameCallback> CallbackFor(
      uint32_t ssrc) const;
  bool HasKey(int key_index) const;
  bool UpdateState(bool sender, webrtc::FrameCryptionState state);

  webrtc::Thread* const signaling_thread_;
  std::unique_ptr<webrtc::Thread> thread_;
  const std::string participant_id_;
  webrtc::scoped_refptr<webrtc::KeyProvider> key_provider_;
  webrtc::scoped_refptr<webrtc::DataPacketCryptor> data_packet_cryptor_;

  mutable webrtc::Mutex mutex_;
  mutable webrtc::Mutex sink_mutex_;
  bool enabled_ RTC_GUARDED_BY(mutex_) = false;
  int key_index_ RTC_GUARDED_BY(mutex_) = 0;
  webrtc::FrameCryptionState last_enc_state_ RTC_GUARDED_BY(mutex_) =
      webrtc::FrameCryptionState::kNew;
  webrtc::FrameCryptionState last_dec_state_ RTC_GUARDED_BY(mutex_) =
      webrtc::FrameCryptionState::kNew;
  webrtc::scoped_refptr<webrtc::FrameCryptorTransformerObserver> observer_
      RTC_GUARDED_BY(mutex_);
  webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_
      RTC_GUARDED_BY(sink_mutex_);
  std::map<uint32_t, webrtc::scoped_refptr<webrtc::TransformedFrameCallback>>
      sink_callbacks_ RTC_GUARDED_BY(sink_mutex_);
};

/// Routes AV1 frames through the codec-aware cryptor and all other media
/// through LiveKit's existing native FrameCryptorTransformer.
class CodecDispatchFrameTransformer : public webrtc::FrameTransformerInterface {
 public:
  CodecDispatchFrameTransformer(
      webrtc::scoped_refptr<webrtc::FrameTransformerInterface> standard,
      webrtc::scoped_refptr<webrtc::FrameTransformerInterface> av1);
  ~CodecDispatchFrameTransformer() override = default;

  void Transform(
      std::unique_ptr<webrtc::TransformableFrameInterface> frame) override;
  void RegisterTransformedFrameCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback)
      override;
  void UnregisterTransformedFrameCallback() override;
  void RegisterTransformedFrameSinkCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
      uint32_t ssrc) override;
  void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override;

 private:
  webrtc::scoped_refptr<webrtc::FrameTransformerInterface> standard_;
  webrtc::scoped_refptr<webrtc::FrameTransformerInterface> av1_;
};

}  // namespace livekit_ffi

#endif  // WEBRTC_AV1_FRAME_CRYPTOR_H_
