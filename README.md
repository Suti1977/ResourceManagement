# ResourceManagement

ResourceManagement is a static-allocation resource management framework for
embedded C applications running on FreeRTOS. It provides lifecycle management,
dependency handling, cooperative execution, software timers, and exclusive
access arbitration.

The framework is split into four modules:

| Module | Purpose | API guide |
|---|---|---|
| **ResourceManager** | Manages resource lifecycles, user requests, dependencies, events, and recovery. | [ResourceManager API](doc/ResourceManager.md) |
| **ResourceLoop** | Runs cooperative resource callbacks sequentially in a dedicated shared FreeRTOS task. | [ResourceLoop API](doc/ResourceLoop.md) |
| **ResourceTimer** | Provides one-shot and periodic software timers for resources attached to a ResourceLoop. | [ResourceTimer API](doc/ResourceTimer.md) |
| **ResourceArbiter** | Grants exclusive access to one resource among multiple priority-based requesters. | [ResourceArbiter API](doc/ResourceArbiter.md) |

## Main features

- Static allocation of framework objects and FreeRTOS synchronization objects.
- Asynchronous resource start and stop lifecycle handling.
- Reference-counted requests from multiple users.
- Dependency graphs with configurable dependency error handling.
- User events and resource-level observers.
- Optional automatic or explicitly requested recovery.
- Generation-aware state reporting for asynchronous workers.
- Cooperative execution of multiple resources in one `ResourceLoop` task.
- Event-driven and timer-driven cooperative callbacks.
- Exclusive resource ownership with priority-based arbitration.

## Requirements

- A C compiler compatible with the application and FreeRTOS port.
- FreeRTOS with static allocation enabled (`configSUPPORT_STATIC_ALLOCATION=1`).
- FreeRTOS headers and the application project headers, including `MyCommon.h`.
- Static storage for manager, resource, user, observer, dependency, loop,
  stack, timer, arbiter, and arbiter request objects for as long as they are used.

The source files are located in `src/`. Add the required source files to the
application build and make `src/` available as an include directory. There is
no build-system-specific project file in this package.

## Module overview and integration

### ResourceManager

`ResourceManager` owns the registered `Resource_t` objects and processes their
lifecycle asynchronously. Applications initialize and register all resources,
users, observers, and dependencies before starting `ResourceManager_run()` in
a dedicated FreeRTOS task. Application tasks request or release resources
through `ResourceUser_t` objects.

See [doc/ResourceManager.md](doc/ResourceManager.md) for initialization order,
resource callbacks, dependency policies, recovery, events, and the complete
public API overview.

### ResourceLoop

`ResourceLoop` is optional. It creates a dedicated statically allocated task
that executes cooperative callbacks for all resources assigned to that loop.
Callbacks for resources in the same loop execute sequentially, not in parallel.
A resource must have a `ResourceLoopExtension_t` and must be added to the loop
before `ResourceManager_run()` starts.

See [doc/ResourceLoop.md](doc/ResourceLoop.md) for task configuration,
`ResourceControl_t`, callback behavior, event masks, and setup examples.

### ResourceTimer

`ResourceTimer` provides timers associated with resources in a `ResourceLoop`.
A timer is registered once and remains in the loop's timer list for the
lifetime of that loop. It can be started, restarted, stopped, and reused, but
there is no timer removal API.

See [doc/ResourceTimer.md](doc/ResourceTimer.md) for timer setup, expiration
semantics, periodic timers, callback registration, and wait-time handling.

### ResourceArbiter

`ResourceArbiter` is independent of the lifecycle manager. Each arbiter
instance controls one exclusive resource. Applications create one request
object per requester and bind it to its arbiter during request initialization.
Request, release, and priority-update operations then use only the request
object. The highest-priority waiting request receives ownership when the
current owner releases it. Calling `ResourceArbiter_release()` on a
waiting request withdraws it from the waiting list. Equal priorities
are served in FIFO order.

See [doc/ResourceArbiter.md](doc/ResourceArbiter.md) for request lifetime,
priority updates, callbacks, clearing requests, and deinitialization.

## Recommended initialization sequence

A typical application follows this order:

1. Statically allocate and initialize a `ResourceManager_t`.
2. Initialize each `Resource_t` using `Resource_init()`.
3. Register each resource using `ResourceManager_addResource()`.
4. For cooperative resources, initialize a `ResourceLoop_t` and add each
   registered resource with `ResourceLoop_addResource()`.
5. Initialize users and observers, and add dependency relationships.
6. Create and register timers for resources already attached to a loop.
7. Optionally register the all-stopped callback.
8. Start `ResourceManager_run()` in its dedicated task.
9. From application tasks, use `Resource_request()` and `Resource_release()`
   through initialized `ResourceUser_t` objects.

Complete examples and API details are provided in the module-specific guides
under `doc/`.

## License

See [LICENSE](LICENSE).
