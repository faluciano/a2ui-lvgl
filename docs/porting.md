# Porting

The libraries are built and tested for the ESP32 family under ESP-IDF and for
Linux hosts. Nothing in `components/` includes an ESP-IDF or FreeRTOS header,
so other platforms should need little work. None have been tried yet. If you
port it, please open an issue or a pull request with what you found.

## What the code needs

| Need | Detail |
|---|---|
| Compiler | C++17. GCC and Clang are tested. Exceptions and RTTI are not used, and the libraries are compiled with them off. |
| Standard library | The hosted C++ standard library: containers, `std::function`, `std::shared_ptr`, `<cmath>`. |
| Heap | The code allocates freely. See the memory table in the README. |
| Floating point | Numbers are 64-bit doubles. Without a double-precision FPU this works, only slower. |
| Clock | `gettimeofday`, `time` and `gmtime_r`, for action timestamps and the date functions. On bare metal, provide `gettimeofday`. |
| Threads | None. Call every function from one thread, the one that runs LVGL. |
| LVGL | 9.6 or later, for `a2ui_lvgl` only. |
| Stack | Validation and resolution recurse, with bounded depth. Give the calling task at least 16 KB. |

`a2ui_core` reads the time of day in one place,
`components/a2ui_core/src/state/surface_model.cpp`. The agent call timeouts
use the value you pass to `MessageProcessor::tick()`, not a system clock.

## Building outside ESP-IDF

Use the root `CMakeLists.txt` (see the README), or compile the sources
directly:

- `components/a2ui_core/src/**/*.cpp`, include path `components/a2ui_core/include`
- `components/a2ui_lvgl/src/**/*.cpp`, include path `components/a2ui_lvgl/include`

The `Makefile` does exactly this for the host tests.

## LVGL configuration

The adapter needs these `lv_conf.h` features: the flex layout and the widgets
`label`, `button`, `textarea`, `checkbox`, `slider`, `roller`, `calendar`,
`span` and `image`.
`test/lvgl/lv_conf.h` is a working host configuration, and
`examples/basic_surface/sdkconfig.defaults` is the ESP-IDF equivalent.

## A different UI toolkit

`a2ui_core` has no LVGL dependency. To render with another toolkit, follow
the upstream framework adapter blueprint
(`third_party/a2ui/blueprints/sdks/a2ui_framework_adapter.blueprint.md`) and
use `components/a2ui_lvgl` as the worked example: `Surface` owns a
`NodeResolver`, and each resolved `ComponentNode` drives one widget.
