# ResourceArbiter API Guide

## 1. Purpose

`ResourceArbiter` grants exclusive access to one resource among multiple
requesters. Each arbiter instance manages one independently arbitrated
resource. The module is independent of the `ResourceManager` lifecycle state
machine; use it when an application needs a single owner for a shared
peripheral, bus, device, or other resource.

The arbiter uses a priority-ordered waiting list. The request with the highest
priority receives ownership when the current owner releases the resource.
Higher numeric values mean higher priority. Equal-priority requests are
served in FIFO order.

Ownership is non-preemptive. A new higher-priority request does not take the
resource away from the current owner.

## 2. Static objects and lifetime

The application provides:

- One persistent `ResourceArbiter_t` per exclusive resource.
- One persistent `ResourceArbiterRequest_t` per requester.
- Persistent callback data objects referenced by `callbackData`.

The request object must remain valid while it is waiting or owns the resource.
It stores a pointer to the arbiter passed to `ResourceArbiterRequest_init()`;
the arbiter must therefore remain valid and initialized for every operation on
the request. To associate an idle request object with a different arbiter,
initialize it again with the new arbiter. No dynamic allocation is performed
by this module. The arbiter creates its
FreeRTOS mutexes using static storage embedded in `ResourceArbiter_t`.

## 3. Initialization and request registration

Initialize the arbiter once with `ResourceArbiter_init()`. Initialize each
request object with `ResourceArbiterRequest_init()`. A non-NULL grant callback
is required.

```c
static ResourceArbiter_t spiArbiter;
static ResourceArbiterRequest_t displayRequest;
static ResourceArbiterRequest_t sensorRequest;

static void SpiGranted(void *callbackData)
{
    Requester_t *requester = callbackData;
    // This requester now owns the SPI resource.
    Requester_startSpiTransfer(requester);
}

void SetupSpiArbiter(void)
{
    ResourceArbiter_init(&spiArbiter);

    ResourceArbiterRequest_init(&displayRequest,
                                &spiArbiter,
                                10U,
                                SpiGranted,
                                &displayRequester);
    ResourceArbiterRequest_init(&sensorRequest,
                                &spiArbiter,
                                5U,
                                SpiGranted,
                                &sensorRequester);
}
```

A request object uses `ResourceArbiterRequestState_t` and starts in
`RESOURCE_ARBITER_REQUEST_IDLE`. While queued it is in
`RESOURCE_ARBITER_REQUEST_WAITING`; ownership is represented by
`RESOURCE_ARBITER_REQUEST_GRANTED`. A request may only be submitted when
it is idle. Do not submit the same request more than once while
it is active or waiting.

## 4. Request and release

The arbiter association is stored in each request object during
`ResourceArbiterRequest_init()`. Submit a request using the request object
alone:

```c
ResourceArbiter_request(&displayRequest);
```

- If the resource is free, the request becomes the owner immediately and its
  grant callback is invoked before `ResourceArbiter_request()` returns.
- If the resource is already owned, the request is inserted into the waiting
  list according to priority. Its callback is not called until it receives
  ownership.

Use the same function to withdraw a waiting request or return an owned
resource:

```c
ResourceArbiter_release(&displayRequest);
```

`ResourceArbiter_release()` handles all three request states:

- `IDLE`: no action is performed. The owner, waiting list, and callbacks are
  unchanged. Repeated release calls are safe.
- `WAITING`: the request is removed from the priority-ordered waiting list,
  its `next` pointer is cleared, and its state changes to `IDLE`. The current
  owner is not affected, and no callback is invoked.
- `GRANTED`: the current owner returns the resource and its request changes
  to `IDLE`. If there are waiting requests, the first request in the
  priority-ordered list becomes the new owner and its grant callback is
  invoked before `ResourceArbiter_release()` returns. If there are no waiting
  requests, the optional idle callback is invoked.

The function obtains the arbiter from the request object. A request in
`WAITING` state can therefore be withdrawn individually without a separate
`cancel()` API. The same call is safe for an `IDLE` request, so callers do
not need to check the request state before releasing it. A waiting request
must be removed through
`ResourceArbiter_release()` rather than by modifying its linked-list pointers
or state directly.

The arbiter does not automatically release ownership when a callback returns.
The owner must call `ResourceArbiter_release()` after completing its use of the
resource, from a later application execution point.

## 5. Priority changes

Change the priority of a waiting request or the current owner with:

```c
ResourceArbiter_setPriority(&sensorRequest, 20U);
```

For a waiting request, the arbiter removes it from the list and reinserts it
according to its new priority. If the new priority equals another request's
priority, the changed request is placed after the requests already at that
priority, preserving FIFO order from the time of the priority change.

Changing the current owner's priority only changes the stored value. It does
not cause preemption and does not transfer ownership.

Only requests in `WAITING` or `GRANTED` state can have their priority changed.
The arbiter is obtained from the request object, so the priority update does
not require a separate arbiter argument. `ResourceArbiter_release()` can
withdraw an individual waiting request, while
`ResourceArbiter_clearRequests()` clears all active and waiting requests.

## 6. Idle callback

An optional callback can be registered with:

```c
static void SpiIdle(void *callbackData)
{
    (void)callbackData;
    // No requester currently owns or waits for this resource.
}

ResourceArbiter_setIdleCallback(&spiArbiter, SpiIdle, applicationData);
```

The idle callback is invoked when the arbiter becomes empty:

- The current owner releases the resource and there are no waiting requests.
- `ResourceArbiter_clearRequests()` removes one or more active requests.

It is not invoked merely because the callback is registered, and it is not
invoked when `clearRequests()` is called on an already empty arbiter. Pass a
`NULL` callback to disable the notification:

```c
ResourceArbiter_setIdleCallback(&spiArbiter, NULL, NULL);
```

The `callbackData` pointer is passed unchanged to the idle callback and must
remain valid while the callback is registered.

## 7. Clear all requests

Use `ResourceArbiter_clearRequests()` to clear the arbiter's internal request
state:

```c
ResourceArbiter_clearRequests(&spiArbiter);
```

The function:

- Removes every waiting request from the linked list.
- Changes every waiting request to `IDLE` and clears its `next` pointer.
- Changes the current owner to `IDLE` and clears the owner pointer.
- Invokes the idle callback once if at least one request existed before the
  operation.

This function only clears the arbiter's internal ownership/request state. It
does not stop a task, cancel an ongoing transfer, or otherwise stop the
application's active work. The application is responsible for coordinating
that work. After clearing, the previous owner request is in `IDLE` state. A later
`ResourceArbiter_release()` call on that request is harmless and performs
no action, but it is not required.

## 8. Callback and concurrency contract

The arbiter uses two mutexes:

- The internal state mutex protects the owner, request states, and waiting
  list.
- The dispatch mutex serializes public API operations with callback execution.

The internal state mutex is released before a callback runs. The dispatch
mutex remains held until the callback returns. Callbacks therefore run
synchronously in the task context that called `request()`, `release()`, or
`clearRequests()`.

Do not call any `ResourceArbiter` public API from either the grant callback or
the idle callback. Do not call another arbiter's public API from these
callbacks either. Keep callbacks short and non-blocking.

The callback receives only its registered `callbackData`. It should record
that ownership was granted or notify the relevant application state machine;
the actual resource operation and later release should be performed by the
application according to its own execution model.

## 9. Deinitialization

Call `ResourceArbiter_deinit()` only after the arbiter is empty:

- `owner` must be `NULL`.
- `waitingList` must be `NULL`.
- No task may be executing an arbiter API or waiting to enter one.

The last condition is an application lifecycle requirement. After deinit, do
not use the arbiter until it has been initialized again.

## 10. Public API reference

| API | Description |
|---|---|
| `ResourceArbiter_init()` | Initialize the arbiter and create its static mutexes. |
| `ResourceArbiter_deinit()` | Deinitialize an empty, inactive arbiter. |
| `ResourceArbiterRequest_init()` | Initialize a request, bind it to an arbiter, and register its grant callback. |
| `ResourceArbiter_request()` | Request ownership using the arbiter stored in the request object. |
| `ResourceArbiter_release()` | Withdraw a waiting request or release the current owner using the arbiter stored in the request object. |
| `ResourceArbiter_clearRequests()` | Clear the owner and all waiting requests. |
| `ResourceArbiter_setIdleCallback()` | Register or disable the callback for the empty state. |
| `ResourceArbiter_setPriority()` | Change the priority of a waiting request or owner using its request object. |
