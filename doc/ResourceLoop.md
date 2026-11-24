# ResourceLoop API Guide

## 1. Purpose

`ResourceLoop` runs cooperative resource callbacks in one dedicated FreeRTOS
task. Multiple resources can share a loop. Their callbacks execute sequentially
in that task, so callbacks belonging to the same loop do not run in parallel.
A long-running or blocking callback delays every other resource assigned to
the same loop.

`ResourceLoop` is optional. Non-cooperative resources can be managed directly
by `ResourceManager` without a loop.

## 2. Static configuration

The loop task uses FreeRTOS static allocation. `ResourceLoopConfig_t` contains:

- `name`: persistent task/diagnostic name.
- `taskPriority`: FreeRTOS task priority.
- `taskStackSize`: stack size in `StackType_t` words, not bytes.
- `taskStackBuffer`: application-provided stack buffer.
- `getTickCount64`: callback returning a monotonically increasing 64-bit tick
  count. The application must provide a source that remains monotonic across
  the underlying tick counter wrap-around.

Static storage for the `ResourceLoop_t`, stack buffer, resources, and each
resource's `ResourceLoopExtension_t` must remain valid for the loop lifetime.
`ResourceLoop_t` embeds a `ResourceTimerManager_t`; the loop initializes
and processes it automatically. `ResourceLoopState_t` is an internal
lifecycle state type and should not be changed by application code.

## 3. Initialization

Initialize the loop with `ResourceLoop_init()`. It initializes its timer
manager and synchronization object, then creates and starts its static task.
The function returns `false` if the configuration is invalid, a value cannot
be represented by the configured FreeRTOS types, or a required object cannot be
created.

A resource must first be initialized and registered with `ResourceManager`,
and its `ResourceConfig_t.loopExtension` must point to a persistent
`ResourceLoopExtension_t`. Attach it with `ResourceLoop_addResource()` before
`ResourceManager_run()` begins.

```c
static ResourceLoop_t applicationLoop;
static StackType_t applicationLoopStack[512];
static ResourceLoopExtension_t uartLoopExtension;

static uint64_t GetTickCount64(void)
{
    // Return an application-maintained 64-bit monotonic tick count.
    return Application_getMonotonicTickCount64();
}

void SetupLoop(void)
{
    ResourceLoopConfig_t loopConfig = {
        .name = "ResourceLoop",
        .taskPriority = tskIDLE_PRIORITY + 2U,
        .taskStackSize = sizeof(applicationLoopStack) /
                         sizeof(applicationLoopStack[0]),
        .taskStackBuffer = applicationLoopStack,
        .getTickCount64 = GetTickCount64
    };

    configASSERT(ResourceLoop_init(&applicationLoop, &loopConfig));
    // After Resource_init() and ResourceManager_addResource():
    configASSERT(ResourceLoop_addResource(&applicationLoop, &uartResource));
}
```

Each resource can belong to at most one loop. `ResourceLoop_addResource()`
requires an initialized loop, an already registered resource, a manager that
has not started, and a non-NULL `loopExtension` that is not already attached.

## 4. Resource callbacks

The cooperative callback functions are configured in `ResourceFuncs_t` and
receive the resource's `ResourceConfig_t.callbackData`:

| Callback | Purpose |
|---|---|
| `startFunc` | Performs or begins the actual resource start. |
| `loopFunc` | Performs one non-blocking work/processing iteration. |
| `stopFunc` | Performs the final stop after stopping is allowed. |
| `errorFunc` | Receives a loop callback error. |
| `errorLoopStartFunc` | Initializes an error-closing sequence. |
| `errorLoopFunc` | Advances the non-blocking error-closing sequence. |

`startFunc`, `stopFunc`, and `loopFunc` return `status_t`. A value other than
`kStatus_Success` enters the resource's error handling path. `errorFunc` is a
notification callback and returns `void`. The return value of
`errorLoopStartFunc` and `errorLoopFunc` are declared with `status_t`
return types, but their return values are ignored by the current loop
implementation. The error-closing sequence is controlled through
`ResourceControl_t.errorClosed` and the manager stop request.

The `startRequestFunc` and `stopRequestFunc` callbacks in `ResourceFuncs_t`
are different: they run from the `ResourceManager` task and forward lifecycle
requests. The cooperative callbacks above run from the `ResourceLoop` task.

All callbacks for all resources attached to the same loop are serialized.
They should be short and non-blocking. Use events and `waitTime` to schedule
future work instead of busy waiting.

## 5. ResourceControl_t

The start and loop callbacks receive a `ResourceControl_t *control`. At the
beginning of each relevant callback, the loop resets the scheduling fields to
their defaults. The callback sets the values for the next iteration.

| Field | Meaning |
|---|---|
| `events` | Application event bits delivered to this callback in this iteration. |
| `waitedEvents` | Application event mask that should trigger a future `loopFunc`. |
| `timed` | `true` when the callback was triggered by a timer deadline. |
| `waitTime` | Delay in FreeRTOS ticks until the next timed iteration. |
| `stopRequest` | Indicates that a stop request is being processed. |
| `done` | Marks the resource's work as complete; the manager receives `RESOURCE_DONE_`. |
| `prohibitStop` | Defers the final stop while the resource completes its own shutdown work. |
| `run` | During startup, indicates whether the resource has reached `RUNNING`. |
| `errorClosed` | Indicates that the error-closing work is complete. |

Important behavior:

- `waitTime = portMAX_DELAY` means no periodic callback deadline is scheduled.
  The loop can still be woken by a matching event or a lifecycle/timer signal.
- `waitTime = 0` schedules an immediate next iteration.
- A non-zero `waitTime` schedules the next callback after that many ticks.
- Only `events & waitedEvents` is passed as `control->events`.
- An application event that does not match `waitedEvents` only wakes the task;
  it does not invoke `loopFunc`, and that input event is consumed.
- `run` is initialized to `true` when startup begins. If `startFunc` leaves
  it true, the resource is reported as `RUNNING`. If startup is asynchronous,
  clear `run` and let `loopFunc` continue the start procedure; set it true when
  startup is complete.
- When a stop is pending, `loopFunc` can perform shutdown work. If
  `prohibitStop` is true, the final `stopFunc` is deferred and `loopFunc` keeps
  running until it clears the flag. Then `stopFunc` runs and the manager is
  informed that the resource is `STOPPED`.
- When `done` is set, the manager is informed that the resource has reached
  `RESOURCE_DONE_`. A stop request arriving at the same time takes precedence.
- An error-closing sequence completes only when the error loop reports
  `errorClosed` and the manager's stop request has also been observed.

## 6. Application events

Use `ResourceLoop_setEvent(resource, events)` from task context to post
application event bits to a loop-attached resource. Use
`ResourceLoop_setEventFromIsr(resource, events)` from ISR context.

The event mask is accumulated with bitwise OR. Multiple occurrences of the
same bit can therefore coalesce. The callback receives only bits selected by
`waitedEvents`. Do not use event bits as a counted message queue.

For a non-loop resource, use `Resource_setEvent()` or
`Resource_setEventFromIsr()` from `ResourceManager.h` instead.

## 7. Public API reference

| API | Description |
|---|---|
| `ResourceLoop_init()` | Initialize the loop and create its statically allocated task. |
| `ResourceLoop_addResource()` | Attach a registered resource before the manager starts. |
| `ResourceLoop_setEvent()` | Post application event bits from task context. |
| `ResourceLoop_setEventFromIsr()` | Post application event bits from ISR context. |

The following functions are marked for internal use by `ResourceManager` and
`ResourceTimer`; application code should not call them directly:

- `ResourceLoop_setStartRequest()`
- `ResourceLoop_setStopRequest()`
- `ResourceLoop_setTimerExpired()`
