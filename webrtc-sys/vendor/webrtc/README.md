# Video sender queue lifetime backport

`rtp_sender_video_frame_transformer_delegate.cc` comes from webrtc-sdk/webrtc
commit `aaeeee8077eb0a4cad1c9494e9c6433c751ef663`, matching the `webrtc-51ef663`
prebuilt selected by webrtc-sys-build 0.3.18. It retains the upstream BSD license.

The only behavioral change backports Mozilla bug 1838080, changeset
`91d9e8b6a5c430a73561ffd2330865f04fcb1a6d`: `Reset()` drains pending transform
tasks after unregistering the callback and clearing the sender. The sender's
owning reference then outlives references held by tasks on the transformation
queue, so the queue cannot be destroyed and joined by its own thread.

The build compiles this source with the same target flags, headers and defines
as the native bridge. It replaces exactly the corresponding object in an
`OUT_DIR` copy of the prebuilt archive. The downloaded/shared archive is never
modified. An unexpected archive layout fails the build instead of silently
falling back to the original object. Revalidate this backport when upgrading
the WebRTC prebuilt; it is temporary until the dependency includes the fix.

Linux x86_64 validation covers all five native `preencoded_loopback` tests,
including repeated encrypted H.264 and AV1 transformer replacement while frames
are flowing. The macOS and Windows archive paths have not been exercised.

```sh
cargo test -p libwebrtc --test preencoded_loopback -- --ignored --test-threads=1
```

Sources:
- https://github.com/livekit/rust-sdks/issues/1632
- https://github.com/webrtc-sdk/webrtc/blob/aaeeee8077eb0a4cad1c9494e9c6433c751ef663/modules/rtp_rtcp/source/rtp_sender_video_frame_transformer_delegate.cc
- https://bugzilla.mozilla.org/show_bug.cgi?id=1838080
- https://hg.mozilla.org/mozilla-central/rev/91d9e8b6a5c4
