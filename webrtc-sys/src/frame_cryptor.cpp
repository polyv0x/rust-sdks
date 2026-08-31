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

#include "livekit/frame_cryptor.h"

#include <memory>
#include <map>
#include <utility>

#include "absl/types/optional.h"
#include "api/make_ref_counted.h"
#include "av1_frame_cryptor.h"
#include "livekit/packet_trailer.h"
#include "livekit/peer_connection.h"
#include "livekit/peer_connection_factory.h"
#include "livekit/webrtc.h"
#include "rtc_base/thread.h"
#include "webrtc-sys/src/frame_cryptor.rs.h"

namespace livekit_ffi {

class ChainedFrameTransformer : public webrtc::FrameTransformerInterface,
                                public webrtc::TransformedFrameCallback {
 public:
  ChainedFrameTransformer(
      webrtc::scoped_refptr<webrtc::FrameTransformerInterface> first,
      webrtc::scoped_refptr<webrtc::FrameTransformerInterface> second)
      : first_(std::move(first)), second_(std::move(second)) {}

  void Transform(
      std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
    first_->Transform(std::move(frame));
  }

  void RegisterTransformedFrameCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback)
      override {
    second_->RegisterTransformedFrameCallback(callback);
    first_->RegisterTransformedFrameCallback(
        webrtc::scoped_refptr<webrtc::TransformedFrameCallback>(this));
  }

  void RegisterTransformedFrameSinkCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
      uint32_t ssrc) override {
    second_->RegisterTransformedFrameSinkCallback(callback, ssrc);
    first_->RegisterTransformedFrameSinkCallback(
        webrtc::scoped_refptr<webrtc::TransformedFrameCallback>(this), ssrc);
  }

  void UnregisterTransformedFrameCallback() override {
    first_->UnregisterTransformedFrameCallback();
    second_->UnregisterTransformedFrameCallback();
  }

  void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override {
    first_->UnregisterTransformedFrameSinkCallback(ssrc);
    second_->UnregisterTransformedFrameSinkCallback(ssrc);
  }

  void OnTransformedFrame(
      std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
    second_->Transform(std::move(frame));
  }

 private:
  webrtc::scoped_refptr<webrtc::FrameTransformerInterface> first_;
  webrtc::scoped_refptr<webrtc::FrameTransformerInterface> second_;
};

class EncodedVideoFrameTapTransformer
    : public webrtc::FrameTransformerInterface {
 public:
  void Transform(
      std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
    webrtc::scoped_refptr<NativeFrameCryptorObserver> observer;
    {
      webrtc::MutexLock lock(&observer_mutex_);
      observer = observer_;
    }
    if (observer &&
        frame->GetDirection() ==
            webrtc::TransformableFrameInterface::Direction::kReceiver) {
      const auto* video_frame =
          static_cast<const webrtc::TransformableVideoFrameInterface*>(
              frame.get());
      observer->OnEncodedVideoFrame(
          frame->GetMimeType(), frame->GetTimestamp(), frame->GetSsrc(),
          video_frame->IsKeyFrame(), frame->GetData());
    }

    auto callback = CallbackFor(frame->GetSsrc());
    if (callback) {
      callback->OnTransformedFrame(std::move(frame));
    }
  }

  void RegisterTransformedFrameCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback)
      override {
    webrtc::MutexLock lock(&callback_mutex_);
    callback_ = std::move(callback);
  }

  void UnregisterTransformedFrameCallback() override {
    webrtc::MutexLock lock(&callback_mutex_);
    callback_ = nullptr;
  }

  void RegisterTransformedFrameSinkCallback(
      webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
      uint32_t ssrc) override {
    webrtc::MutexLock lock(&callback_mutex_);
    sink_callbacks_[ssrc] = std::move(callback);
  }

  void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override {
    webrtc::MutexLock lock(&callback_mutex_);
    sink_callbacks_.erase(ssrc);
  }

  void SetObserver(
      webrtc::scoped_refptr<NativeFrameCryptorObserver> observer) {
    webrtc::MutexLock lock(&observer_mutex_);
    observer_ = std::move(observer);
  }

 private:
  webrtc::scoped_refptr<webrtc::TransformedFrameCallback> CallbackFor(
      uint32_t ssrc) const {
    webrtc::MutexLock lock(&callback_mutex_);
    auto it = sink_callbacks_.find(ssrc);
    return it == sink_callbacks_.end() ? callback_ : it->second;
  }

  mutable webrtc::Mutex callback_mutex_;
  mutable webrtc::Mutex observer_mutex_;
  webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_
      RTC_GUARDED_BY(callback_mutex_);
  std::map<uint32_t, webrtc::scoped_refptr<webrtc::TransformedFrameCallback>>
      sink_callbacks_ RTC_GUARDED_BY(callback_mutex_);
  webrtc::scoped_refptr<NativeFrameCryptorObserver> observer_
      RTC_GUARDED_BY(observer_mutex_);
};

webrtc::FrameCryptorTransformer::Algorithm AlgorithmToFrameCryptorAlgorithm(
    Algorithm algorithm) {
  switch (algorithm) {
    case Algorithm::AesGcm:
      return webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
    case Algorithm::AesCbc:
      return webrtc::FrameCryptorTransformer::Algorithm::kAesCbc;
    default:
      return webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
  }
}

webrtc::KeyDerivationAlgorithm
KeyDerivationAlgorithmToFrameCryptorKeyDerivationAlgorithm(
    KeyDerivationAlgorithm algorithm) {
  switch (algorithm) {
    case KeyDerivationAlgorithm::PBKDF2:
      return webrtc::KeyDerivationAlgorithm::kPBKDF2;
    case KeyDerivationAlgorithm::HKDF:
      return webrtc::KeyDerivationAlgorithm::kHKDF;
    default:
      return webrtc::KeyDerivationAlgorithm::kPBKDF2;
  }
}

KeyProvider::KeyProvider(KeyProviderOptions options) {
  webrtc::KeyProviderOptions rtc_options;
  rtc_options.shared_key = options.shared_key;

  std::vector<uint8_t> ratchet_salt;
  std::copy(options.ratchet_salt.begin(), options.ratchet_salt.end(),
            std::back_inserter(ratchet_salt));

  rtc_options.ratchet_salt = ratchet_salt;
  rtc_options.ratchet_window_size = options.ratchet_window_size;
  rtc_options.failure_tolerance = options.failure_tolerance;
  rtc_options.key_ring_size = options.key_ring_size;
  rtc_options.key_derivation_algorithm =
      KeyDerivationAlgorithmToFrameCryptorKeyDerivationAlgorithm(
          options.key_derivation_algorithm);
  impl_ =
      new webrtc::RefCountedObject<webrtc::DefaultKeyProviderImpl>(rtc_options);
}

FrameCryptor::FrameCryptor(
    std::shared_ptr<RtcRuntime> rtc_runtime,
    const std::string participant_id,
    webrtc::FrameCryptorTransformer::Algorithm algorithm,
    webrtc::scoped_refptr<webrtc::KeyProvider> key_provider,
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender)
    : rtc_runtime_(rtc_runtime),
      participant_id_(participant_id),
      key_provider_(key_provider),
      sender_(sender) {
  auto mediaType =
      sender->track()->kind() == "audio"
          ? webrtc::FrameCryptorTransformer::MediaType::kAudioFrame
          : webrtc::FrameCryptorTransformer::MediaType::kVideoFrame;
  e2ee_transformer_ = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(
      new webrtc::FrameCryptorTransformer(rtc_runtime->signaling_thread(),
                                          participant_id, mediaType, algorithm,
                                          key_provider_));
  av1_e2ee_transformer_ = webrtc::make_ref_counted<Av1FrameCryptorTransformer>(
      rtc_runtime->signaling_thread(), participant_id, algorithm,
      key_provider_);
  crypto_transformer_ = webrtc::make_ref_counted<CodecDispatchFrameTransformer>(
      e2ee_transformer_, av1_e2ee_transformer_);
  sender->SetEncoderToPacketizerFrameTransformer(crypto_transformer_);
  e2ee_transformer_->SetEnabled(false);
  av1_e2ee_transformer_->SetEnabled(false);
}

FrameCryptor::FrameCryptor(
    std::shared_ptr<RtcRuntime> rtc_runtime,
    const std::string participant_id,
    webrtc::FrameCryptorTransformer::Algorithm algorithm,
    webrtc::scoped_refptr<webrtc::KeyProvider> key_provider,
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver)
    : rtc_runtime_(rtc_runtime),
      participant_id_(participant_id),
      key_provider_(key_provider),
      receiver_(receiver) {
  auto mediaType =
      receiver->track()->kind() == "audio"
          ? webrtc::FrameCryptorTransformer::MediaType::kAudioFrame
          : webrtc::FrameCryptorTransformer::MediaType::kVideoFrame;
  e2ee_transformer_ = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(
      new webrtc::FrameCryptorTransformer(rtc_runtime->signaling_thread(),
                                          participant_id, mediaType, algorithm,
                                          key_provider_));
  av1_e2ee_transformer_ = webrtc::make_ref_counted<Av1FrameCryptorTransformer>(
      rtc_runtime->signaling_thread(), participant_id, algorithm,
      key_provider_);
  crypto_transformer_ = webrtc::make_ref_counted<CodecDispatchFrameTransformer>(
      e2ee_transformer_, av1_e2ee_transformer_);
  if (mediaType ==
      webrtc::FrameCryptorTransformer::MediaType::kVideoFrame) {
    encoded_video_frame_tap_ =
        webrtc::make_ref_counted<EncodedVideoFrameTapTransformer>();
    receiver_transformer_ = webrtc::make_ref_counted<ChainedFrameTransformer>(
        crypto_transformer_, encoded_video_frame_tap_);
  } else {
    receiver_transformer_ = crypto_transformer_;
  }
  receiver->SetDepacketizerToDecoderFrameTransformer(receiver_transformer_);
  e2ee_transformer_->SetEnabled(false);
  av1_e2ee_transformer_->SetEnabled(false);
}

FrameCryptor::~FrameCryptor() {
  if (observer_) {
    unregister_observer();
  }
}

void FrameCryptor::register_observer(
    rust::Box<RtcFrameCryptorObserverWrapper> observer) const {
  webrtc::MutexLock lock(&mutex_);
  observer_ = webrtc::make_ref_counted<NativeFrameCryptorObserver>(
      std::move(observer), this);
  e2ee_transformer_->RegisterFrameCryptorTransformerObserver(observer_);
  av1_e2ee_transformer_->RegisterObserver(observer_);
}

void FrameCryptor::unregister_observer() const {
  webrtc::MutexLock lock(&mutex_);
  if (encoded_video_frame_tap_) {
    encoded_video_frame_tap_->SetObserver(nullptr);
  }
  av1_e2ee_transformer_->UnregisterObserver();
  observer_ = nullptr;
  e2ee_transformer_->UnRegisterFrameCryptorTransformerObserver();
}

void FrameCryptor::set_encoded_video_frame_observer_enabled(
    bool enabled) const {
  webrtc::MutexLock lock(&mutex_);
  if (encoded_video_frame_tap_) {
    encoded_video_frame_tap_->SetObserver(enabled ? observer_ : nullptr);
  }
}

void FrameCryptor::set_packet_trailer_handler(
    std::shared_ptr<PacketTrailerHandler> handler) const {
  if (!handler) {
    return;
  }

  auto timestamp_transformer = handler->transformer();
  if (!timestamp_transformer) {
    return;
  }

  webrtc::scoped_refptr<webrtc::FrameTransformerInterface> first;
  webrtc::scoped_refptr<webrtc::FrameTransformerInterface> second;
  if (sender_) {
    first = crypto_transformer_;
    second = timestamp_transformer;
  } else if (receiver_) {
    first = timestamp_transformer;
    second = receiver_transformer_;
  } else {
    return;
  }

  chained_transformer_ =
      webrtc::make_ref_counted<ChainedFrameTransformer>(first, second);

  if (sender_) {
    sender_->SetEncoderToPacketizerFrameTransformer(chained_transformer_);
  }
  if (receiver_) {
    receiver_->SetDepacketizerToDecoderFrameTransformer(chained_transformer_);
  }
}

NativeFrameCryptorObserver::NativeFrameCryptorObserver(
    rust::Box<RtcFrameCryptorObserverWrapper> observer,
    const FrameCryptor* fc)
    : observer_(std::move(observer)), fc_(fc) {}

NativeFrameCryptorObserver::~NativeFrameCryptorObserver() {}

void NativeFrameCryptorObserver::OnFrameCryptionStateChanged(
    const std::string participant_id,
    webrtc::FrameCryptionState state) {
  observer_->on_frame_cryption_state_change(
      participant_id, static_cast<FrameCryptionState>(state));
}

void NativeFrameCryptorObserver::OnEncodedVideoFrame(
    const std::string mime_type,
    uint32_t timestamp,
    uint32_t ssrc,
    bool key_frame,
    webrtc::ArrayView<const uint8_t> data) {
  rust::Vec<uint8_t> owned_data;
  owned_data.reserve(data.size());
  std::copy(data.begin(), data.end(), std::back_inserter(owned_data));
  observer_->on_encoded_video_frame(mime_type, timestamp, ssrc, key_frame,
                                    std::move(owned_data));
}

void FrameCryptor::set_enabled(bool enabled) const {
  webrtc::MutexLock lock(&mutex_);
  e2ee_transformer_->SetEnabled(enabled);
  av1_e2ee_transformer_->SetEnabled(enabled);
}

bool FrameCryptor::enabled() const {
  webrtc::MutexLock lock(&mutex_);
  return e2ee_transformer_->enabled() && av1_e2ee_transformer_->enabled();
}

void FrameCryptor::set_key_index(int32_t index) const {
  webrtc::MutexLock lock(&mutex_);
  e2ee_transformer_->SetKeyIndex(index);
  av1_e2ee_transformer_->SetKeyIndex(index);
}

int32_t FrameCryptor::key_index() const {
  webrtc::MutexLock lock(&mutex_);
  return e2ee_transformer_->key_index();
}

DataPacketCryptor::DataPacketCryptor(
    webrtc::FrameCryptorTransformer::Algorithm algorithm,
    webrtc::scoped_refptr<webrtc::KeyProvider> key_provider)
    : data_packet_cryptor_(
          webrtc::make_ref_counted<webrtc::DataPacketCryptor>(algorithm,
                                                              key_provider)) {}

EncryptedPacket DataPacketCryptor::encrypt_data_packet(
    const ::rust::String participant_id,
    uint32_t key_index,
    rust::Vec<::std::uint8_t> data) const {
  std::vector<uint8_t> data_vec;
  std::copy(data.begin(), data.end(), std::back_inserter(data_vec));

  auto result = data_packet_cryptor_->Encrypt(
      std::string(participant_id.data(), participant_id.size()), key_index,
      data_vec);

  if (!result.ok()) {
    throw std::runtime_error(std::string("Failed to encrypt data packet: ") +
                             result.error().message());
  }

  auto& packet = result.value();

  EncryptedPacket encrypted_packet;
  encrypted_packet.data = rust::Vec<uint8_t>();
  std::copy(packet->data.begin(), packet->data.end(),
            std::back_inserter(encrypted_packet.data));

  encrypted_packet.iv = rust::Vec<uint8_t>();
  std::copy(packet->iv.begin(), packet->iv.end(),
            std::back_inserter(encrypted_packet.iv));

  encrypted_packet.key_index = packet->key_index;

  return encrypted_packet;
}

rust::Vec<::std::uint8_t> DataPacketCryptor::decrypt_data_packet(
    const ::rust::String participant_id,
    const EncryptedPacket& encrypted_packet) const {
  std::vector<uint8_t> data_vec;
  std::copy(encrypted_packet.data.begin(), encrypted_packet.data.end(),
            std::back_inserter(data_vec));

  std::vector<uint8_t> iv_vec;
  std::copy(encrypted_packet.iv.begin(), encrypted_packet.iv.end(),
            std::back_inserter(iv_vec));

  auto native_encrypted_packet =
      webrtc::make_ref_counted<webrtc::EncryptedPacket>(
          std::move(data_vec), std::move(iv_vec), encrypted_packet.key_index);

  auto result = data_packet_cryptor_->Decrypt(
      std::string(participant_id.data(), participant_id.size()),
      native_encrypted_packet);

  if (!result.ok()) {
    throw std::runtime_error(std::string("Failed to decrypt data packet: ") +
                             result.error().message());
  }

  rust::Vec<uint8_t> decrypted_data;
  auto& decrypted = result.value();
  std::copy(decrypted.begin(), decrypted.end(),
            std::back_inserter(decrypted_data));
  return decrypted_data;
}

std::shared_ptr<KeyProvider> new_key_provider(KeyProviderOptions options) {
  return std::make_shared<KeyProvider>(options);
}

std::shared_ptr<FrameCryptor> new_frame_cryptor_for_rtp_sender(
    std::shared_ptr<PeerConnectionFactory> peer_factory,
    const ::rust::String participant_id,
    Algorithm algorithm,
    std::shared_ptr<KeyProvider> key_provider,
    std::shared_ptr<RtpSender> sender) {
  return std::make_shared<FrameCryptor>(
      peer_factory->rtc_runtime(),
      std::string(participant_id.data(), participant_id.size()),
      AlgorithmToFrameCryptorAlgorithm(algorithm),
      key_provider->rtc_key_provider(), sender->rtc_sender());
}

std::shared_ptr<FrameCryptor> new_frame_cryptor_for_rtp_receiver(
    std::shared_ptr<PeerConnectionFactory> peer_factory,
    const ::rust::String participant_id,
    Algorithm algorithm,
    std::shared_ptr<KeyProvider> key_provider,
    std::shared_ptr<RtpReceiver> receiver) {
  return std::make_shared<FrameCryptor>(
      peer_factory->rtc_runtime(),
      std::string(participant_id.data(), participant_id.size()),
      AlgorithmToFrameCryptorAlgorithm(algorithm),
      key_provider->rtc_key_provider(), receiver->rtc_receiver());
}

std::shared_ptr<DataPacketCryptor> new_data_packet_cryptor(
    Algorithm algorithm,
    std::shared_ptr<KeyProvider> key_provider) {
  return std::make_shared<DataPacketCryptor>(
      AlgorithmToFrameCryptorAlgorithm(algorithm),
      key_provider->rtc_key_provider());
}

}  // namespace livekit_ffi
