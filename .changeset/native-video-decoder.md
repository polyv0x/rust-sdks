---
"webrtc-sys": minor
"libwebrtc": minor
---

Add an application-supplied native video decoder contract with asynchronous decoded callbacks,
owned native buffers, callback revocation, truthful decoder information, and WebRTC software
fallback. Applications can retain their native allocation lease through NativeVideoStream
without an implicit I420 conversion. Codec libraries and IPC remain application-owned.
