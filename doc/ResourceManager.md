# ResourceManager API Guide

## 1. Purpose

`ResourceManager` manages the lifecycle of registered resources. It tracks
resource users, reference counts, dependencies, pending work, events, and
recovery operations. Resource lifecycle transitions are processed
asynchronously by the manager task.

The manager does not create its own task. The application must run
`ResourceManager_run()` from one dedicated FreeRTOS task. That function is a
permanent processing loop and is intended to be called once for that manager.

All manager, resource, user, observer, and dependency objects are supplied by
the application. Their storage must remain valid while the manager uses them.
Registration lists are built during initialization and must not be modified
after `ResourceManager_run()` has started.

## 2. Main objects

- `ResourceManager_t`: owns the resource list and the pending-resource queue.
- `Resource_t`: represents one managed resource and its lifecycle state.
- `ResourceUser_t`: represents one application's request for a resource.
- `ResourceObserver_t`: observes resource events without requesting the
  resource or changing its reference count.
- `ResourceDependency_t`: declares that one resource requires another.
- `ResourceLoopExtension_t`: optional per-resource state used when the
  resource runs in a `ResourceLoop`.

Keep every object in static or otherwise long-lived storage. Names passed in
configuration structures are also retained as pointers and must remain valid.

## 3. Initialization order

Complete the following steps before the manager task is started:

1. Initialize the manager with `ResourceManager_init()`.
2. Initialize each resource with `Resource_init()`.
3. Register each initialized resource with `ResourceManager_addResource()`.
4. If cooperative execution is required, initialize a `ResourceLoop_t` and
   attach eligible registered resources with `ResourceLoop_addResource()`.
5. Initialize users with `ResourceUser_init()`.
6. Optionally initialize observers with `ResourceObserver_init()`.
7. Add dependency relationships with `Resource_addDependency()`.
8. Optionally register an all-stopped callback.
9. Start `ResourceManager_run()` in its dedicated task.

`ResourceManager_addResource()`, `ResourceUser_init()`,
`ResourceObserver_init()`, and `Resource_addDependency()` are initialization-
phase operations. Do not call them after `ResourceManager_run()` has started.
`ResourceObserver_init()` specifically does not acquire the manager mutex, so
it must not race with starting or running the manager.

## 4. Basic setup example

The example below shows a non-cooperative resource. The start and stop
callbacks report lifecycle state changes to the manager. In a real application
these callbacks may start or stop hardware or launch an asynchronous worker.

```c
#include "ResourceManager.h"

static ResourceManager_t manager;
static Resource_t uartResource;
static ResourceUser_t applicationUser;

static status_t Uart_startRequest(Resource_t *resource,
                                  void *callbackData)
{
    (void)callbackData;

    // Replace with the hardware start sequence.
    Resource_setState(resource, RESOURCE_RUNNING, kStatus_Success);
    return kStatus_Success;
}

static status_t Uart_stopRequest(Resource_t *resource,
                                 void *callbackData)
{
    (void)callbackData;

    // Replace with the hardware stop sequence.
    Resource_setState(resource, RESOURCE_STOPPED, kStatus_Success);
    return kStatus_Success;
}

void Application_resourceSetup(void)
{
    ResourceConfig_t resourceConfig = {0};
    ResourceUserConfig_t userConfig = {0};

    configASSERT(ResourceManager_init(&manager));

    resourceConfig.name = "UART";
    resourceConfig.funcs.startRequestFunc = Uart_startRequest;
    resourceConfig.funcs.stopRequestFunc = Uart_stopRequest;
    resourceConfig.loopExtension = NULL;

    configASSERT(Resource_init(&uartResource, &resourceConfig));
    configASSERT(ResourceManager_addResource(&manager, &uartResource));

    userConfig.name = "Application";
    userConfig.resource = &uartResource;
    configASSERT(ResourceUser_init(&applicationUser, &userConfig));

    // Create a dedicated task that calls ResourceManager_run(&manager).
}

void Application_useUart(void)
{
    Resource_request(&applicationUser);
}

void Application_releaseUart(void)
{
    Resource_release(&applicationUser);
}
```

A `Resource_request()` call changes the user's request flag and increments the
resource reference count. It does not synchronously start the resource. The
manager task processes the pending lifecycle work. `Resource_release()` clears
the user's request and decrements the reference count; the resource stops
asynchronously when no requests remain and dependencies permit the transition.
Both operations are idempotent for an individual user: a repeated request does
not add another reference, and a repeated release does not decrement again.

## 5. Resource configuration and callbacks

The main public configuration and state types are `ResourceConfig_t`,
`ResourceFuncs_t`, `ResourceUserConfig_t`, `ResourceObserverConfig_t`,
`ResourceControl_t`, and `ResourceErrorInfo_t`. The `Resource_t` state field
uses `ResourceState_t`; application code should use the state reporting and
query functions rather than modifying resource fields directly.

`ResourceConfig_t` contains:

- `name`: persistent diagnostic name.
- `funcs`: callback table (`ResourceFuncs_t`).
- `callbackData`: application data passed to the resource callbacks.
- `recoveryMaxAttempts`: maximum recovery attempts; `0` means unlimited.
- `loopExtension`: `NULL` for a non-loop resource, or a persistent
  `ResourceLoopExtension_t` for a cooperative resource.

### Non-loop resources

Use `startRequestFunc`, `stopRequestFunc`, and optionally `eventFunc`.

- `startRequestFunc` runs in the `ResourceManager` task and forwards the start
  request to the application's implementation. It must return
  `kStatus_Success` on success. A different status reports a resource error.
- If a non-loop resource has no `startRequestFunc`, the manager immediately
  treats it as `RESOURCE_RUNNING` when start is requested.
- If `startRequestFunc` is present, the implementation must report the
  resulting state using `Resource_setState()` or
  `Resource_setStateForGeneration()`.
- `stopRequestFunc` runs in the manager task. If it is absent, the manager
  does not call an application stop callback; the resource implementation
  must report its terminal state itself.
- `eventFunc` receives application events for a non-loop resource and runs in
  the manager task.

### Cooperative resources

A cooperative resource must have a statically allocated
`ResourceLoopExtension_t`, and that resource must be attached to a successfully
initialized `ResourceLoop_t` before the manager starts. Its callbacks
`startFunc`, `stopFunc`, `loopFunc`, `errorFunc`, `errorLoopStartFunc`, and
`errorLoopFunc` are executed by the `ResourceLoop` task.

`startRequestFunc` and `stopRequestFunc`, when provided, still run in the
manager task to forward lifecycle requests. The `ResourceLoop` then executes
the cooperative callbacks.

Do not set loop-only callbacks when `loopExtension` is `NULL`;
`Resource_init()` rejects that configuration.

The callback typedefs declared in `ResourceManager.h` are:

| Type | Signature/purpose |
|---|---|
| `Resource_startRequest_fn_t` | `status_t(Resource_t *, void *)`; forwards a start request from the manager task. |
| `Resource_stopRequest_fn_t` | `status_t(Resource_t *, void *)`; forwards a stop request from the manager task. |
| `Resource_event_fn_t` | `void(Resource_t *, uint64_t, void *)`; receives application events for a non-loop resource. |
| `Resource_start_fn_t` | `status_t(void *, ResourceControl_t *)`; performs or begins cooperative startup. |
| `Resource_stop_fn_t` | `status_t(void *)`; performs the final cooperative stop. |
| `Resource_loop_fn_t` | `status_t(void *, ResourceControl_t *)`; performs one cooperative iteration. |
| `Resource_error_fn_t` | `void(void *, status_t)`; receives a cooperative callback error. |
| `Resource_errorLoopStart_fn_t` | `status_t(void *, ResourceControl_t *)`; initializes error closing; its return value is ignored by the current implementation. |
| `Resource_errorLoop_fn_t` | `status_t(void *, ResourceControl_t *)`; advances error closing; its return value is ignored by the current implementation. |
| `Resource_event_cb_t` | User event callback described in the next section. |
| `Resource_observer_cb_t` | Resource-level event observer callback. |
| `ResourceManager_all_stopped_cb_t` | Manager notification when all resources are fully stopped. |

## 6. Users and events

A `ResourceUser_t` is initialized with `ResourceUser_init()` and a
`ResourceUserConfig_t` containing a persistent name, the target resource, an
optional event callback, and callback data.

The user callback type is:

```c
typedef void (*Resource_event_cb_t)(ResourceUser_t *user,
                                    ResourceEvent_t event,
                                    const ResourceErrorInfo_t *errorInfo,
                                    void *callbackData);
```

The `ResourceEvent_t` user events are:

- `RESOURCE_EVENT_STARTED`: the resource is available in `RUNNING` state.
- `RESOURCE_EVENT_STOPPED`: the resource has stopped.
- `RESOURCE_EVENT_ERROR`: an error was reported.
- `RESOURCE_EVENT_RECOVERY`: recovery has started.
- `RESOURCE_EVENT_RECOVERED`: recovery completed successfully.
- `RESOURCE_EVENT_DONE`: the resource reported that its work is complete.
- `RESOURCE_EVENT_RELEASED`: this user released its request.

The user callback runs while the manager's recursive mutex is held. The API
explicitly supports calling `Resource_request()`, `Resource_release()`, and
`Resource_recover()` from this callback. Keep callback execution short and do
not block waiting for work that requires the manager task. `errorInfo` is
non-NULL for error events and is only valid for the duration of the callback;
do not retain the pointer.

`ResourceObserver_t` is initialized with `ResourceObserver_init()` and a
`ResourceObserverConfig_t`. An observer receives the resource event once per
event, independently of the number of active users. It does not create a
resource request. Its registration is also restricted to initialization.

## 7. Dependencies

Add a dependency using a caller-owned, persistent `ResourceDependency_t`:

```c
static ResourceDependency_t networkDependency;

Resource_addDependency(&networkDependency,
                       &applicationResource,
                       &networkResource,
                       RESOURCE_DEP_PROPAGATE);
```

The first resource is the dependent resource; the second is the resource it
requires. Both resources must already belong to the same manager. A dependency
relationship can only be added during initialization. The dependency graph
must be acyclic. Cycle checking is disabled by default for performance; it can
be enabled with `RESOURCE_MANAGER_ENABLE_DEPENDENCY_CYCLE_CHECK=1`.

The `ResourceDependencyErrorStrategy_t` error strategies are:

- `RESOURCE_DEP_IGNORE`: an error in the dependency does not block the
  dependent resource.
- `RESOURCE_DEP_PROPAGATE`: dependency errors are propagated to the dependent
  resource.
- `RESOURCE_DEP_AUTO_RECOVER`: the manager requests recovery of the failed
  dependency. If its recovery attempt limit is exhausted, the error is
  propagated.

The dependency is held by an internal user. Its request contributes to the
dependency resource's reference count like any other user request.

The convenience macros `CREATE_RESOURCE_DEPENDENCY_()` and
`CREATE_RESOURCE_DEPENDENCY()` create a static dependency object at the call
site. The first uses `RESOURCE_DEP_PROPAGATE`; the second accepts an explicit
`ResourceDependencyErrorStrategy_t`.

## 8. Recovery and error reporting

Automatic recovery is disabled for users by default. Enable it per user with:

```c
ResourceUser_setAutoRecovery(&applicationUser, true);
```

This setting can be changed at runtime. Enabling it can trigger recovery for
an already active self or propagated dependency error. Disabling it does not
cancel a recovery request that has already been issued.

`Resource_recover()` requests explicit recovery for a user. The request is
processed asynchronously by the manager.

`Resource_setRecoveryPolicy()` selects the resource's default policy:

- `RESOURCE_RECOVERY_POLICY_STOP`: after an error, stop and remain stopped.
- `RESOURCE_RECOVERY_POLICY_AUTO`: automatically restart after an error,
  subject to the recovery override and attempt limit. These values belong to
  the `ResourceRecoveryPolicy_t` type.

A reported error can override the default policy using
`ResourceRecoveryOverride_t`:

- `RESOURCE_RECOVERY_OVERRIDE_DEFAULT`: use the resource's configured policy.
- `RESOURCE_RECOVERY_OVERRIDE_DISABLED`: disable recovery for the error
  process.
- `RESOURCE_RECOVERY_OVERRIDE_FORCED`: force recovery.

The `ResourceRecoveryOverride_t` strength is ordered
`RESOURCE_RECOVERY_OVERRIDE_FORCED > RESOURCE_RECOVERY_OVERRIDE_DISABLED >
RESOURCE_RECOVERY_OVERRIDE_DEFAULT` while an error process is active.

Use `Resource_reportError(reporter, faultSource, status, recoveryOverride)`
to report an error. `faultSource` identifies the resource that originally
failed and must be in the reporter's dependency tree. The first accepted
failure information is retained; the recovery override may be strengthened
while the error process is active. The function returns `true` if the report
was accepted.

For asynchronous workers, use
`Resource_reportErrorForGeneration()` with the lifecycle generation captured
for the operation. This prevents a late error from an earlier lifecycle from
affecting a newer start/stop cycle.

## 9. Lifecycle state reporting and generations

The `ResourceState_t` values are:

- `RESOURCE_STOPPED`
- `RESOURCE_STARTING`
- `RESOURCE_RUNNING`
- `RESOURCE_STOPPING_`
- `RESOURCE_ERROR_`
- `RESOURCE_WAITING_FOR_DEPENDENCY`
- `RESOURCE_DONE_`

Synchronous callback implementations can report state using
`Resource_setState()`. For asynchronous workers, capture the generation using
`Resource_getLifecycleGeneration()` before starting the asynchronous work, pass
that value to the worker, and report completion with
`Resource_setStateForGeneration()`.

Example pattern:

```c
uint32_t generation = Resource_getLifecycleGeneration(resource);
// Pass generation to the asynchronous operation.

// Later, from the worker completion path:
Resource_setStateForGeneration(resource,
                               generation,
                               RESOURCE_RUNNING,
                               kStatus_Success);
```

A report from an old generation is discarded. For
`Resource_setStateForGeneration()` and `Resource_setState()`, use a non-success
status for `RESOURCE_ERROR_`; other states require `kStatus_Success`.

## 10. Application events

`Resource_setEvent()` posts a 64-bit application event mask from task context.
`Resource_setEventFromIsr()` is the ISR-safe form. For a resource attached to a
`ResourceLoop`, events are delivered to that loop; otherwise an optional
`eventFunc` is called from the manager task.

The event mask is bitwise-ORed, so repeated occurrences of the same bit can
coalesce. If individual event occurrences must be preserved, use a separate
queue or message mechanism in the application.

## 11. All-stopped callback

Register at most one all-stopped callback before `ResourceManager_run()`:

```c
static void AllStopped(ResourceManager_t *manager, void *callbackData)
{
    (void)manager;
    (void)callbackData;
    // Handle the fully stopped condition.
}

ResourceManager_registerAllStoppedCallback(&manager,
                                           AllStopped,
                                           applicationData);
```

It runs outside the manager mutex when all registered resources are
`RESOURCE_STOPPED`, all reference counts are zero, no lifecycle callback is
active, and the pending list is empty. It is not called on startup merely
because all resources initially are stopped. It is called after an active
resource cycle has fully stopped; it can be called again after a later cycle.

## 12. Diagnostic reason types

The manager exposes three reason enums for diagnostics:

- `ResourceStartReason_t`: `RESOURCE_START_REASON_NORMAL` and
  `RESOURCE_START_REASON_RECOVERY`.
- `ResourceErrorReason_t`: `RESOURCE_ERROR_REASON_NONE`,
  `RESOURCE_ERROR_REASON_SELF_FAULT`,
  `RESOURCE_ERROR_REASON_DEPENDENCY_FAULT`, and
  `RESOURCE_ERROR_REASON_REPORTED_FAULT`.
- `ResourceRecoveryReason_t`: `RESOURCE_RECOVERY_REASON_NONE`,
  `RESOURCE_RECOVERY_REASON_SELF_FAULT`,
  `RESOURCE_RECOVERY_REASON_DEPENDENCY`, and
  `RESOURCE_RECOVERY_REASON_HIGHER_LAYER_REQUEST`.

`ResourceFlags_t` and `ResourceLoopState_t` are implementation state types
exposed in the header because the resource and loop extension structures are
visible. Applications should not modify these fields directly.

## 13. Public API reference

| API | Description |
|---|---|
| `ResourceManager_init()` | Initialize the manager and its static synchronization objects. |
| `Resource_init()` | Initialize a resource and copy its callback configuration. |
| `ResourceManager_addResource()` | Register a resource during initialization. |
| `ResourceManager_getResourceByName()` | Find a registered resource by name; returns `NULL` if absent. |
| `ResourceManager_registerAllStoppedCallback()` | Register the optional all-stopped notification before run. |
| `ResourceUser_init()` | Attach a user to a registered resource during initialization. |
| `ResourceObserver_init()` | Register a resource event observer during initialization. |
| `Resource_addDependency()` | Add a same-manager dependency during initialization. |
| `ResourceUser_setAutoRecovery()` | Enable or disable a user's automatic recovery request. |
| `Resource_request()` / `Resource_release()` | Assert or withdraw a user's resource request. |
| `Resource_recover()` | Request recovery for a user. |
| `Resource_reportError()` | Report an error against a resource in the reporter's dependency tree. |
| `Resource_reportErrorForGeneration()` | Report an error only if the lifecycle generation still matches. |
| `Resource_setRecoveryPolicy()` | Set the resource's default recovery policy. |
| `Resource_getStartReason()` | Read whether the latest start is normal or recovery-related. |
| `Resource_getErrorReason()` | Read the last error reason and optionally copy its details. |
| `Resource_getRecoveryReason()` | Read the reason for the current recovery. |
| `Resource_recoveryFailed()` | Check whether the recovery attempt limit was exhausted. |
| `Resource_getLifecycleGeneration()` | Get the current lifecycle generation for asynchronous work. |
| `Resource_setStateForGeneration()` | Report state from a generation-tagged asynchronous operation. |
| `Resource_setState()` | Report state without generation validation. |
| `Resource_setEvent()` / `Resource_setEventFromIsr()` | Post application events from task or ISR context. |
| `ResourceManager_run()` | Run the manager's permanent processing loop in its dedicated task. |
| `ResourceManager_printStatus()` | Print resource state and optional user/dependency details. |
| `Resource_getState()` / `Resource_getRefCount()` | Read resource state and active request count. |
| `ResourceUser_isRequested()` | Read whether a user currently requests its resource. |
| `ResourceUser_recoveryRequested()` | Read whether a user has an explicit recovery request. |
| `Resource_stateTosStr()` / `Resource_eventTosStr()` | Convert state and event values to diagnostic strings. |

The optional development-only `Resource_start()` and `Resource_stop()` APIs
are available only when `RESOURCE_MANAGER_ENABLE_DEVELOPMENT_API=1` is defined.
They directly change the resource reference count and are intended for
console/debug use, not normal application code. Pair every `Resource_start()`
with a `Resource_stop()`.
