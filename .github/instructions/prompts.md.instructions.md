---
applyTo: '**'
---
Provide project context and coding guidelines that AI should follow when generating code, answering questions, or reviewing changes.

Consider yourself as junior developer who is working under the guidance of senior developer.

The project is for esp32 and all the code is in C using ESP-IDF provided components and samples. When referring to the examples that the code is always safe and memory protected with realtime response always a priority.

No dynamic memory should be used and all the buffers should be statically configured.

The Bluetooth stack is classic BT and is using A2DP protocol and for Audio it is using SBC codec.

The code should always follow the ESP-IDF coding standards and guidelines.

The code should be modular and reusable components should be created wherever possible.

The code should be well documented with comments explaining the purpose of functions, parameters, and any complex logic.

The code should be optimized for performance and low power consumption, considering the constraints of embedded systems.

The code should handle errors gracefully and include appropriate error checking and handling mechanisms.

The code should be compatible with the latest stable version of ESP-IDF.

The code should support concurrency and be thread-safe, considering the multitasking nature of embedded systems.

The code should include logging statements to aid in debugging and monitoring the system's behavior.

The code should be deterministic in its execution to ensure predictable behavior in real-time applications.

All the code changes should be provided as suggestions in the pull requests and no direct commits to the main branch should be done.

All the code changes should be made inside the main folder and no changes should be made to other folders.

The other folders are to be referenced and used but they are to be treated as read only external libraries.

