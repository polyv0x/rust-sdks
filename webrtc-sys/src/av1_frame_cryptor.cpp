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

#include "av1_frame_cryptor.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "api/make_ref_counted.h"
#include "av1_bitstream.h"
#include "rtc_base/logging.h"

namespace livekit_ffi {
namespace {

constexpr size_t kGcmTagSize = 16;
constexpr size_t kGcmIvSize = 12;

std::vector<uint8_t> ToVector(webrtc::ArrayView<const uint8_t> data) {
  return std::vector<uint8_t>(data.begin(), data.end());
}

}  // namespace

bool IsAv1Frame(const webrtc::TransformableFrameInterface& frame) {
  std::string mime_type = frame.GetMimeType();
  std::transform(
      mime_type.begin(), mime_type.end(), mime_type.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return mime_type.find("av1") != std::string::npos;
}

Av1FrameCryptorTransformer::Av1FrameCryptorTransformer(
    webrtc::Thread* signaling_thread,
    std::string participant_id,
    webrtc::FrameCryptorTransformer::Algorithm algorithm,
    webrtc::scoped_refptr<webrtc::KeyProvider> key_provider)
    : signaling_thread_(signaling_thread),
      thread_(webrtc::Thread::Create()),
      participant_id_(std::move(participant_id)),
      key_provider_(std::move(key_provider)),
      data_packet_cryptor_(
          webrtc::make_ref_counted<webrtc::DataPacketCryptor>(algorithm,
                                                              key_provider_)) {
  RTC_DCHECK(signaling_thread_ != nullptr);
  RTC_DCHECK(key_provider_ != nullptr);
  thread_->SetName("Av1FrameCryptorTransformer", this);
  thread_->Start();
}

Av1FrameCryptorTransformer::~Av1FrameCryptorTransformer() {
  thread_->Stop();
}

void Av1FrameCryptorTransformer::Transform(
    std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
  switch (frame->GetDirection()) {
    case webrtc::TransformableFrameInterface::Direction::kSender:
      thread_->PostTask([this, frame = std::move(frame)]() mutable {
        EncryptFrame(std::move(frame));
      });
      break;
    case webrtc::TransformableFrameInterface::Direction::kReceiver:
      thread_->PostTask([this, frame = std::move(frame)]() mutable {
        DecryptFrame(std::move(frame));
      });
      break;
    case webrtc::TransformableFrameInterface::Direction::kUnknown:
      RTC_LOG(LS_WARNING) << "AV1 E2EE received a frame with unknown direction";
      break;
  }
}

void Av1FrameCryptorTransformer::RegisterTransformedFrameCallback(
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) {
  webrtc::MutexLock lock(&sink_mutex_);
  callback_ = std::move(callback);
}

void Av1FrameCryptorTransformer::UnregisterTransformedFrameCallback() {
  webrtc::MutexLock lock(&sink_mutex_);
  callback_ = nullptr;
}

void Av1FrameCryptorTransformer::RegisterTransformedFrameSinkCallback(
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
    uint32_t ssrc) {
  webrtc::MutexLock lock(&sink_mutex_);
  sink_callbacks_[ssrc] = std::move(callback);
}

void Av1FrameCryptorTransformer::UnregisterTransformedFrameSinkCallback(
    uint32_t ssrc) {
  webrtc::MutexLock lock(&sink_mutex_);
  sink_callbacks_.erase(ssrc);
}

void Av1FrameCryptorTransformer::SetEnabled(bool enabled) {
  webrtc::MutexLock lock(&mutex_);
  enabled_ = enabled;
}

bool Av1FrameCryptorTransformer::enabled() const {
  webrtc::MutexLock lock(&mutex_);
  return enabled_;
}

void Av1FrameCryptorTransformer::SetKeyIndex(int index) {
  webrtc::MutexLock lock(&mutex_);
  key_index_ = index;
}

int Av1FrameCryptorTransformer::key_index() const {
  webrtc::MutexLock lock(&mutex_);
  return key_index_;
}

void Av1FrameCryptorTransformer::RegisterObserver(
    webrtc::scoped_refptr<webrtc::FrameCryptorTransformerObserver> observer) {
  webrtc::MutexLock lock(&mutex_);
  observer_ = std::move(observer);
}

void Av1FrameCryptorTransformer::UnregisterObserver() {
  webrtc::MutexLock lock(&mutex_);
  observer_ = nullptr;
}

webrtc::scoped_refptr<webrtc::TransformedFrameCallback>
Av1FrameCryptorTransformer::CallbackFor(uint32_t ssrc) const {
  webrtc::MutexLock lock(&sink_mutex_);
  auto it = sink_callbacks_.find(ssrc);
  return it == sink_callbacks_.end() ? callback_ : it->second;
}

bool Av1FrameCryptorTransformer::HasKey(int key_index) const {
  if (key_index < 0 || key_index >= key_provider_->options().key_ring_size) {
    return false;
  }
  auto key_handler = key_provider_->options().shared_key
                         ? key_provider_->GetSharedKey(participant_id_)
                         : key_provider_->GetKey(participant_id_);
  return key_handler != nullptr && key_handler->GetKeySet(key_index) != nullptr;
}

bool Av1FrameCryptorTransformer::UpdateState(bool sender,
                                             webrtc::FrameCryptionState state) {
  webrtc::scoped_refptr<webrtc::FrameCryptorTransformerObserver> observer;
  {
    webrtc::MutexLock lock(&mutex_);
    webrtc::FrameCryptionState& previous =
        sender ? last_enc_state_ : last_dec_state_;
    if (previous == state) {
      return false;
    }
    previous = state;
    observer = observer_;
  }
  if (observer) {
    signaling_thread_->PostTask([observer = std::move(observer), state,
                                 participant_id = participant_id_]() mutable {
      observer->OnFrameCryptionStateChanged(participant_id, state);
    });
  }
  return true;
}

void Av1FrameCryptorTransformer::EncryptFrame(
    std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
  auto callback = CallbackFor(frame->GetSsrc());
  if (!callback) {
    if (UpdateState(true, webrtc::FrameCryptionState::kInternalError)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE sender has no frame callback";
    }
    return;
  }

  const auto data_in = frame->GetData();
  if (data_in.empty()) {
    if (!key_provider_->options().discard_frame_when_cryptor_not_ready) {
      callback->OnTransformedFrame(std::move(frame));
    }
    return;
  }
  if (!enabled()) {
    callback->OnTransformedFrame(std::move(frame));
    return;
  }

  const int current_key_index = key_index();
  if (current_key_index > 255 || !HasKey(current_key_index)) {
    if (UpdateState(true, webrtc::FrameCryptionState::kMissingKey)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE sender is missing key index "
                          << current_key_index;
    }
    return;
  }

  std::vector<uint8_t> output = ToVector(data_in);
  livekit::av1::E2eeEncryptionLayout layout;
  std::vector<uint8_t> protected_bytes;
  if (!livekit::av1::ComputeE2eeEncryptionLayout(output.data(), output.size(),
                                                 &layout) ||
      !livekit::av1::ExtractProtectedBytes(output.data(), output.size(), layout,
                                           &protected_bytes)) {
    if (UpdateState(true, webrtc::FrameCryptionState::kInternalError)) {
      RTC_LOG(LS_WARNING)
          << "AV1 E2EE could not determine the sender encryption layout";
    }
    return;
  }

  auto encrypted = data_packet_cryptor_->Encrypt(
      participant_id_, current_key_index, protected_bytes);
  if (!encrypted.ok()) {
    if (UpdateState(true, webrtc::FrameCryptionState::kEncryptionFailed)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE encryption failed: "
                          << encrypted.error().message();
    }
    return;
  }

  const auto& packet = encrypted.value();
  if (packet->iv.size() != kGcmIvSize ||
      packet->data.size() != layout.protected_size + kGcmTagSize ||
      !livekit::av1::WriteProtectedBytes(&output, layout, packet->data.data(),
                                         layout.protected_size)) {
    if (UpdateState(true, webrtc::FrameCryptionState::kInternalError)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE produced an invalid encrypted packet";
    }
    return;
  }

  livekit::av1::E2eeMetadata metadata;
  metadata.key_index = packet->key_index;
  metadata.iv = packet->iv;
  metadata.tag.assign(packet->data.end() - kGcmTagSize, packet->data.end());
  if (!livekit::av1::AppendE2eeMetadataObu(&output, metadata)) {
    if (UpdateState(true, webrtc::FrameCryptionState::kInternalError)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE could not append frame metadata";
    }
    return;
  }

  frame->SetData(webrtc::ArrayView<const uint8_t>(output));
  UpdateState(true, webrtc::FrameCryptionState::kOk);
  callback->OnTransformedFrame(std::move(frame));
}

void Av1FrameCryptorTransformer::DecryptFrame(
    std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
  auto callback = CallbackFor(frame->GetSsrc());
  if (!callback) {
    if (UpdateState(false, webrtc::FrameCryptionState::kInternalError)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE receiver has no frame callback";
    }
    return;
  }

  const auto data_in = frame->GetData();
  if (data_in.empty()) {
    if (!key_provider_->options().discard_frame_when_cryptor_not_ready) {
      callback->OnTransformedFrame(std::move(frame));
    }
    return;
  }
  if (!enabled()) {
    callback->OnTransformedFrame(std::move(frame));
    return;
  }

  const auto& magic = key_provider_->options().uncrypted_magic_bytes;
  if (!magic.empty() && data_in.size() >= magic.size() &&
      std::equal(magic.begin(), magic.end(), data_in.end() - magic.size())) {
    std::vector<uint8_t> unencrypted(data_in.begin(),
                                     data_in.end() - magic.size());
    frame->SetData(webrtc::ArrayView<const uint8_t>(unencrypted));
    callback->OnTransformedFrame(std::move(frame));
    return;
  }

  std::vector<uint8_t> output;
  livekit::av1::E2eeMetadata metadata;
  if (!livekit::av1::ExtractE2eeMetadataObu(data_in.data(), data_in.size(),
                                            &output, &metadata)) {
    if (UpdateState(false, webrtc::FrameCryptionState::kDecryptionFailed)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE metadata is missing or malformed";
    }
    return;
  }
  if (!HasKey(metadata.key_index)) {
    if (UpdateState(false, webrtc::FrameCryptionState::kMissingKey)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE receiver is missing key index "
                          << static_cast<int>(metadata.key_index);
    }
    return;
  }

  livekit::av1::E2eeEncryptionLayout layout;
  std::vector<uint8_t> encrypted_data;
  if (!livekit::av1::ComputeE2eeEncryptionLayout(output.data(), output.size(),
                                                 &layout) ||
      !livekit::av1::ExtractProtectedBytes(output.data(), output.size(), layout,
                                           &encrypted_data)) {
    if (UpdateState(false, webrtc::FrameCryptionState::kInternalError)) {
      RTC_LOG(LS_WARNING)
          << "AV1 E2EE could not determine the receiver encryption layout";
    }
    return;
  }
  encrypted_data.insert(encrypted_data.end(), metadata.tag.begin(),
                        metadata.tag.end());
  auto encrypted_packet = webrtc::make_ref_counted<webrtc::EncryptedPacket>(
      std::move(encrypted_data), metadata.iv, metadata.key_index);
  auto decrypted =
      data_packet_cryptor_->Decrypt(participant_id_, encrypted_packet);
  if (!decrypted.ok()) {
    if (UpdateState(false, webrtc::FrameCryptionState::kDecryptionFailed)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE decryption failed: "
                          << decrypted.error().message();
    }
    return;
  }

  const std::vector<uint8_t>& protected_bytes = decrypted.value();
  if (!livekit::av1::WriteProtectedBytes(
          &output, layout, protected_bytes.data(), protected_bytes.size())) {
    if (UpdateState(false, webrtc::FrameCryptionState::kInternalError)) {
      RTC_LOG(LS_WARNING) << "AV1 E2EE decrypted an invalid payload size";
    }
    return;
  }

  frame->SetData(webrtc::ArrayView<const uint8_t>(output));
  UpdateState(false, webrtc::FrameCryptionState::kOk);
  callback->OnTransformedFrame(std::move(frame));
}

CodecDispatchFrameTransformer::CodecDispatchFrameTransformer(
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> standard,
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> av1)
    : standard_(std::move(standard)), av1_(std::move(av1)) {}

void CodecDispatchFrameTransformer::Transform(
    std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
  if (IsAv1Frame(*frame)) {
    av1_->Transform(std::move(frame));
  } else {
    standard_->Transform(std::move(frame));
  }
}

void CodecDispatchFrameTransformer::RegisterTransformedFrameCallback(
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) {
  standard_->RegisterTransformedFrameCallback(callback);
  av1_->RegisterTransformedFrameCallback(std::move(callback));
}

void CodecDispatchFrameTransformer::UnregisterTransformedFrameCallback() {
  standard_->UnregisterTransformedFrameCallback();
  av1_->UnregisterTransformedFrameCallback();
}

void CodecDispatchFrameTransformer::RegisterTransformedFrameSinkCallback(
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback,
    uint32_t ssrc) {
  standard_->RegisterTransformedFrameSinkCallback(callback, ssrc);
  av1_->RegisterTransformedFrameSinkCallback(std::move(callback), ssrc);
}

void CodecDispatchFrameTransformer::UnregisterTransformedFrameSinkCallback(
    uint32_t ssrc) {
  standard_->UnregisterTransformedFrameSinkCallback(ssrc);
  av1_->UnregisterTransformedFrameSinkCallback(ssrc);
}

}  // namespace livekit_ffi
