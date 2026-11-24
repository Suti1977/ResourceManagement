# ResourceTimer API Guide

## 1. Purpose

`ResourceTimer` provides one-shot and periodic software timers for resources
attached to a `ResourceLoop`. The timer manager is integrated into the loop
and is processed by the loop task. Timer callbacks therefore execute in the
`ResourceLoop` task context.

The timer implementation uses a 64-bit absolute tick value. The associated
`ResourceLoop_t` obtains the current time through its `getTickCount64` callback.

## 2. Timer object and lifetime

The application supplies each `ResourceTimer_t` object in static or otherwise
long-lived storage. A timer is registered once with
`ResourceTimer_createTimer()` and remains in the loop's timer list for the
entire loop lifetime. There is no timer removal or delete API.

Do not call `ResourceTimer_createTimer()` more than once for the same timer
object. The caller is responsible for ensuring one-time registration. A timer
must not be reused with another resource or loop after registration.

A timer can be stopped and restarted any number of times after registration.

## 3. Create and register a timer

A resource must have a `ResourceLoopExtension_t` and must already be attached
to an initialized loop using `ResourceLoop_addResource()`.

```c
static ResourceTimer_t statusTimer;

static void StatusTimer_expired(void *callbackData)
{
    (void)callbackData;
    // Runs in the ResourceLoop task.
    Application_handleStatusTimeout();
}

void SetupStatusTimer(void)
{
    configASSERT(ResourceTimer_createTimer(&uartResource, &statusTimer));

    ResourceTimer_registerExpiredFunc(&statusTimer,
                                      StatusTimer_expired,
                                      NULL);

    // First expiration after 1000 ticks, then every 1000 ticks.
    ResourceTimer_start(&statusTimer, 1000U, 1000U);
}
```

`ResourceLoop_init()` initializes its internal `ResourceTimerManager_t`.
Applications normally do not need to initialize or run that manager directly.

## 4. Start and stop

```c
ResourceTimer_start(&statusTimer, interval, periodTime);
ResourceTimer_stop(&statusTimer);
```

Both values are in FreeRTOS ticks:

- `interval` is the delay until the first expiration.
- `periodTime == 0` creates a one-shot timer.
- `periodTime != 0` creates a periodic timer.
- Starting an active timer restarts it from the current time.
- `interval == 0` sets the expired indication immediately; the loop processes
  the expiration in its next timer processing cycle.

Periodic timers advance from their previous scheduled deadline, not from the
callback completion time. A delayed loop iteration does not cause the timer
to execute multiple callbacks in a burst within one manager processing
iteration.

Stopping a timer deactivates it and clears any unread expiration indication.
It does not remove the timer object from the manager list. The timer can be
started again later.

## 5. Expiration indication and callback

The optional expiration callback is registered with:

```c
ResourceTimer_registerExpiredFunc(&statusTimer,
                                  StatusTimer_expired,
                                  callbackData);
```

The callback type is `ResourceTimer_expired_fn_t`. The callback is invoked
from the `ResourceLoop` task when the timer expires. The `callbackData`
pointer must remain valid for the timer/loop lifetime.
The callback can start or stop timers; the timer manager releases its internal
list lock before invoking the callback.

The expiration indication can also be polled:

```c
if (ResourceTimer_expired(&statusTimer))
{
    // Consume the expiration indication.
}
```

`ResourceTimer_expired()` returns the current indication and clears it. The
same indication should therefore be treated as a one-time notification. If an
expiration callback is registered, it is independent of whether the application
also reads the `expired` flag.

Use `ResourceTimer_isActive()` to check whether the timer is currently active.

## 6. Timing integration

`ResourceTimer_getWaitTime()` returns the number of ticks until the next timer
deadline. It returns `portMAX_DELAY` when there is no active deadline, zero
when a deadline is already due, and caps long finite delays to a value below
`portMAX_DELAY` so they can be waited in chunks.

The loop uses this information when selecting its next task wait interval.
Applications using timers through `ResourceLoop` should not call
`ResourceTimer_runManager()` themselves; the loop invokes it as part of its
normal processing.

`ResourceTimer_initManager()` and `ResourceTimer_runManager()` are lower-level
manager operations exposed by the header. In the standard integrated setup,
`ResourceLoop` owns and drives the timer manager.

## 7. Public API reference

| API | Description |
|---|---|
| `ResourceTimer_initManager()` | Initialize an empty timer manager. Normally called by `ResourceLoop_init()`. |
| `ResourceTimer_createTimer()` | Register a timer once for a resource already attached to a loop. |
| `ResourceTimer_start()` | Start or restart a one-shot or periodic timer. |
| `ResourceTimer_stop()` | Stop a timer and clear its unread expiration flag. |
| `ResourceTimer_expired()` | Read and clear the expiration indication. |
| `ResourceTimer_isActive()` | Check whether the timer is active. |
| `ResourceTimer_registerExpiredFunc()` | Register or replace the timer expiration callback and callback data. |
| `ResourceTimer_runManager()` | Process timers up to an absolute 64-bit time; normally called by `ResourceLoop`. |
| `ResourceTimer_getWaitTime()` | Get the tick delay until the next timer deadline. |
