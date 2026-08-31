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

#ifndef WEBRTC_AV1_BITSTREAM_H_
#define WEBRTC_AV1_BITSTREAM_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace livekit {
namespace av1 {

/// Parsed span of a single AV1 OBU inside a low-overhead bitstream.
struct ObuSpan {
  size_t offset = 0;
  size_t total_size = 0;
  int type = -1;
  bool has_size_field = false;
};

/// A byte range inside a low-overhead AV1 temporal unit. The end offset is
/// exclusive.
struct ByteRange {
  size_t start = 0;
  size_t end = 0;
};

/// The AV1 bytes that may be encrypted without hiding the OBU framing and
/// frame-type bits required by WebRTC and an SFU.
struct E2eeEncryptionLayout {
  std::vector<ByteRange> protected_ranges;
  size_t protected_size = 0;
};

/// Metadata carried in the final AV1 OBU_METADATA for an encrypted frame.
struct E2eeMetadata {
  uint8_t key_index = 0;
  std::vector<uint8_t> iv;
  std::vector<uint8_t> tag;
};

/// Parse AV1 OBUs using the same rules as WebRTC's `RtpPacketizerAv1`.
/// Returns an empty vector when the bitstream is malformed.
std::vector<ObuSpan> ParseObus(const uint8_t* data, size_t len);

/// Returns true when the bitstream contains an `OBU_SEQUENCE_HEADER`.
bool HasSequenceHeaderObu(const uint8_t* data, size_t len);

/// Extract the first sequence-header OBU bytes, if present.
bool ExtractSequenceHeaderObu(const uint8_t* data,
                              size_t len,
                              std::vector<uint8_t>* out);

/// Prepend a cached sequence-header OBU to a keyframe when the encoder omitted
/// it.
void EnsureSequenceHeaderOnKeyframe(
    std::vector<uint8_t>* packet,
    const std::vector<uint8_t>& cached_seq_header);

/// Strip a per-frame IVF container header when present.
void StripIvfFrameHeaderIfPresent(std::vector<uint8_t>* packet);

/// Convert AV1 Annex-B temporal/frame/OBU units to low-overhead OBUs when
/// present.
void ConvertAnnexBToLowOverheadIfPresent(std::vector<uint8_t>* packet);

/// Strip OBUs that should not be transferred in WebRTC RTP payloads when
/// present.
void StripNonTransferObusIfPresent(std::vector<uint8_t>* packet);

/// Normalizes an AV1 temporal unit for WebRTC RTP packetization: strips IVF
/// framing, converts Annex-B units to low-overhead OBUs, and strips
/// non-transfer OBUs. Shared by every encoder that emits AV1 into the RTP
/// pipeline so the steps cannot drift apart.
void NormalizeForRtp(std::vector<uint8_t>* packet);

/// Basic validation that WebRTC's AV1 packetizer can parse the bitstream.
bool IsWebRtcParseable(const uint8_t* data, size_t len);

/// Compute the selectively encrypted ranges used by LiveKit AV1 E2EE. OBU
/// headers, extension/size fields, and the first payload byte of frame-header
/// OBUs remain clear so RTP packetization and SFU keyframe detection keep
/// working.
bool ComputeE2eeEncryptionLayout(const uint8_t* data,
                                 size_t len,
                                 E2eeEncryptionLayout* layout);

/// Gather or replace the protected bytes described by `layout`.
bool ExtractProtectedBytes(const uint8_t* data,
                           size_t len,
                           const E2eeEncryptionLayout& layout,
                           std::vector<uint8_t>* protected_bytes);
bool WriteProtectedBytes(std::vector<uint8_t>* data,
                         const E2eeEncryptionLayout& layout,
                         const uint8_t* protected_bytes,
                         size_t protected_len);

/// Append/extract the standards-compliant OBU_METADATA envelope used by
/// LiveKit's AV1 E2EE proposal. Extraction also removes the metadata OBU from
/// the returned payload.
bool AppendE2eeMetadataObu(std::vector<uint8_t>* data,
                           const E2eeMetadata& metadata);
bool ExtractE2eeMetadataObu(const uint8_t* data,
                            size_t len,
                            std::vector<uint8_t>* payload,
                            E2eeMetadata* metadata);

}  // namespace av1
}  // namespace livekit

#endif  // WEBRTC_AV1_BITSTREAM_H_
