# API Documentation

The ResourceManagement package is divided into four modules. Each module has
its own API guide:

- [ResourceManager](ResourceManager.md) — resource lifecycle, users,
  dependencies, events, and recovery.
- [ResourceLoop](ResourceLoop.md) — cooperative resource execution and
  `ResourceControl_t` behavior.
- [ResourceTimer](ResourceTimer.md) — one-shot and periodic timers integrated
  with a `ResourceLoop`.
- [ResourceArbiter](ResourceArbiter.md) — priority-based exclusive resource
  access and ownership callbacks.

The guides describe the API in the corresponding `src/*.h` files. The headers
remain the authoritative source for declarations and types.
