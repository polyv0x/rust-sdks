---
"webrtc-sys": patch
---

Drain pending video frame transformations before releasing the sender delegate.
This prevents native video publication/reconfiguration from destroying a task
queue on its own thread and aborting with `pthread_join` / `EDEADLK`.
