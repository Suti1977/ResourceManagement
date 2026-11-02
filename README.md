# ResourceManagement
A lightweight, static-allocation resource management framework for embedded C and FreeRTOS, featuring dependency management, asynchronous resource lifecycle handling, event-driven cooperative execution, error handling, and software timers.

A lightweight resource management framework for embedded systems written in C and designed for FreeRTOS.

The framework provides:

- **ResourceManager** — manages resource lifecycles, dependencies, start and stop requests, and error propagation.
- **ResourceLoop** — supports cooperative execution of multiple resources within a shared task, with event-driven processing.
- **ResourceTimer** — provides software timers integrated with the resource loop.

The framework is designed for predictable memory usage, using static allocation instead of dynamic memory allocation. It supports asynchronous resource lifecycle management, event callbacks, and structured error handling.

Suitable for embedded applications that require reliable coordination of interdependent services and resources.