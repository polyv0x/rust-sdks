---
"webrtc-sys": patch
"libwebrtc": minor
"livekit-ffi": patch
---

Report macOS H.264 decoder acceleration from the active VideoToolbox session,
including hardware, software, and unavailable measurements. Retain WebRTC's
existing H.264 decoding and native frame delivery behavior. Session replacement
and teardown invalidate measurements, and legacy Objective-C decoder defaults no
longer claim software decoding in SDK statistics.

`InboundRtpStreamStats::power_efficient_decoder` now has type `Option<bool>` so
Rust consumers can distinguish an unavailable measurement from software decoding.
The existing protobuf boolean retains its legacy false default.
