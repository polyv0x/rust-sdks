---
webrtc-sys: patch
libwebrtc: patch
livekit: patch
---

Dispatch pre-encoded video at the first-frame input boundary instead of relying
on asynchronous encoder-selector feedback. This prevents native encoded buffers
from reaching macOS pixel encoders and crashing in Objective-C frame conversion.
Preserve pixel encoding, software native-buffer conversion, and encoder control
feedback. Add deterministic native factory regressions behind `test-support`.
