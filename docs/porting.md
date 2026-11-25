# Porting

The libraries are built and tested for the ESP32 family under ESP-IDF and for
Linux hosts. Nothing in `components/` includes an ESP-IDF or FreeRTOS header,
so other platforms should need little work. None have been tried yet. If you
port it, please open an issue or a pull request with what you found.

## What the code needs

| Need | Detail |
|---|---|
| Compiler | C++17. GCC and Clang are tested. Exceptions and RTTI are not used, and the libraries are compiled with them off. |
| Standard library | The hosted C++ standard library: containers, `std::function`, `<memory>`, `<cmath>`. Shared ownership uses `a2ui::Ref` (see [Reference counting](#reference-counting)). |
| Heap | The code allocates freely, mostly in small blocks. The basic catalog takes about 17 KB in its compact form, 150 KB parsed into RAM. See [Memory on ESP-IDF](#memory-on-esp-idf). |
| Floating point | Numbers are 64-bit doubles. Without a double-precision FPU this works, only slower. |
| Clock | `gettimeofday`, `time` and `gmtime_r`, for action timestamps and the date functions. On bare metal, provide `gettimeofday`. |
| Threads | None. Call every function from one thread, the one that runs LVGL. |
| LVGL | 9.6 or later, for `a2ui_lvgl` only. |
| Stack | Validation and resolution recurse, with bounded depth. A typical card needs about 9 KB; a message at the default depth limits about 36 KB. See [Stack](#stack). |

`a2ui_core` reads the time of day in one place,
`components/a2ui_core/src/state/surface_model.cpp`. The agent call timeouts
use the value you pass to `MessageProcessor::tick()`, not a system clock.

## Memory on ESP-IDF

The figures in this section were measured with ESP-IDF v6.0.3 and the example's
`sdkconfig.defaults` (`-Os`), in Espressif's QEMU emulation of the ESP32-S3
(octal PSRAM) and the ESP32-C3. The library runs on an ESP32-P4 board, but
these figures have not been measured on hardware yet.
[`tools/stack_probe`](../tools/stack_probe/README.md) reproduces the stack
figures, in QEMU or on a board.

### Reference counting

The libraries share ownership through `a2ui::Ref<T>` (`a2ui/ref.h`). With
libstdc++, the standard library ESP-IDF uses, it is
`std::__shared_ptr<T, __gnu_cxx::_S_single>`: the `std::shared_ptr` interface
with plain integer reference counts, which is enough because the libraries run
on one task.

The libraries avoid `std::shared_ptr` because of how ESP-IDF builds libstdc++
for the ESP32-S2 and the RISC-V chips (ESP32-C2, C3, C5, C6, H2 and P4). There
it uses the mutex lock policy, so every `std::shared_ptr` control block creates
a FreeRTOS mutex in internal RAM and keeps it until the object is freed. The
ESP32 and ESP32-S3 toolchains use atomics and are not affected.

When the libraries still used `std::shared_ptr`, loading the basic catalog and
showing the coffee-order card created 422 mutexes, all alive at once (counted
on a host build configured like the RISC-V toolchain). At roughly 100 bytes
each on the ESP32-P4 (an 84-byte FreeRTOS queue, an 8-byte wrapper and
allocator overhead), that is about 40 KB of internal RAM. With `a2ui::Ref`
the same card creates one, for the adapter's per-surface style cache.

You do not need to configure anything, and your own code can keep using
`std::shared_ptr`; the image loader's request handle is a
`std::shared_ptr<void>`. Do not work around the lock policy with
`-D_GLIBCXX_HAVE_ATOMIC_LOCK_POLICY=1`: it changes libstdc++'s ABI for the
whole project, and code compiled into the prebuilt libstdc++ (`std::future`,
for example) would no longer match.

### The compact catalog

`a2ui::basic_catalog::load()` loads the basic catalog and binds its functions,
in one of two forms. The last row is the loading code 0.1.0 documented, for
comparison.

| Form | How | Heap (ESP32-C3) | Flash |
|---|---|---|---|
| Compact | `load(true)`, or `load()` with `CONFIG_A2UI_COMPACT_CATALOG=y` | 16.5 KB | 44 KB of tables |
| Parsed into RAM | `load(false)`, or `load()` by default | 150 KB | |
| The 0.1.0 snippet | `Catalog::from_json(*spec::parsed(...))` | 225 KB | |

The compact form is `Catalog::from_packed(*spec::packed(Document::BasicCatalogV1))`.
`tools/embed_spec_documents.py` flattens the catalog and the common types
into constant tables (`PackedDocument`, `a2ui/schema_node.h`): nodes of two
words each, and one pool of strings. The validator reads the schemas from
the tables in place, so only the summary of each component and function
(names, property kinds, child slots) is on the heap. The summaries are
computed by the same code as for a parsed catalog, and `make test` runs the
conformance suites with every catalog packed, so validation gives the same
results.

What changes in a compact catalog: `ComponentApi::schema`,
`FunctionApi::schema`, `args_schema`, `description` and `Catalog::source()`
are undefined. Read schemas through `schema_node()`, `args_schema_node()`
and `source_node()`, which work for both forms. `Catalog::to_json()` returns
the same document as for a parsed catalog.

A catalog that arrives at run time can be packed too: `a2ui::PackedBuffer`
builds the tables from a `Json`, and `Catalog::from_packed()` loads them. The
buffer must outlive the catalog. It is on the heap, but smaller than the
parsed `Json` (for the basic catalog, 28 KB against an estimated 80 KB on an
ESP32), so free the `Json` once the catalog is loaded.

The parsed form saves 75 KB over the 0.1.0 snippet because `load(false)`
moves a temporary parse into the catalog (`Catalog::from_json(Json&&)`)
instead of copying the cached `spec::parsed()` document. It also parses the
common types (about 25 KB) on first use; the compact catalog uses their
packed form.

`make heap-report` prints host figures for the three ways of loading (64-bit,
so larger than on an ESP32):

```
Catalog and processor: 253884 B in 4249 blocks (host, 64-bit)   0.1.0 snippet
Catalog and processor: 169802 B in 2645 blocks (host, 64-bit)   load(false)
Catalog and processor: 25873 B in 146 blocks (host, 64-bit)     load(true)
```

### Internal RAM and PSRAM

| What | Heap |
|---|---|
| Basic catalog, compact | about 17 KB |
| Basic catalog, parsed into RAM | about 150 KB |
| A typical card (`13_coffee-order`), with its LVGL widgets | about 80 KB |

Almost every allocation is smaller than a few hundred bytes. With
`CONFIG_SPIRAM_USE_MALLOC`, ESP-IDF serves a `malloc` smaller than
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16384 by default, 4096 in the example)
from internal RAM first, and only spills into PSRAM once internal RAM runs
out. On an ESP32-S3 with 8 MB of PSRAM and the example's settings, the
0.1.0 catalog and the coffee-order card took about 300 KB of internal RAM,
leaving about 80 KB, and a second, deeper card used up the rest. The compact
catalog would need about 200 KB less; that has not been measured on the
ESP32-S3.

There are two ways to move this into PSRAM:

- **For the whole application**, set `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`.
  Every `malloc` then prefers PSRAM; in the measurement above, internal RAM
  stayed almost untouched. PSRAM is slower than internal RAM, and this also
  moves other components' allocations, including LVGL's objects when
  `CONFIG_LV_USE_CLIB_MALLOC` is set. Drivers that need internal or DMA memory
  ask for it with `heap_caps_malloc` and keep working;
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` sets aside internal RAM for them.
- **For the A2UI task only**, replace the global `operator new` so that
  allocations made on that task prefer PSRAM. The libraries allocate through
  `new`, so this covers them, while LVGL's own `malloc` calls are unaffected:

  ```cpp
  #include <cstdlib>
  #include <new>

  #include "esp_heap_caps.h"
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"

  // Set to the task that runs A2UI and LVGL, before it processes anything.
  TaskHandle_t g_a2ui_task = nullptr;

  void* operator new(std::size_t size) {
    if (size == 0) size = 1;
    if (g_a2ui_task != nullptr && xTaskGetCurrentTaskHandle() == g_a2ui_task) {
      if (void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
    }
    if (void* p = std::malloc(size)) return p;
    std::abort();  // Built without exceptions: out of memory is fatal.
  }
  void* operator new[](std::size_t size) { return operator new(size); }
  // free() releases memory from any ESP-IDF heap, PSRAM included.
  void operator delete(void* p) noexcept { std::free(p); }
  void operator delete[](void* p) noexcept { std::free(p); }
  void operator delete(void* p, std::size_t) noexcept { std::free(p); }
  void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
  ```

**Chips without PSRAM** need the compact catalog. On the ESP32-C3 (316 KB of
heap at start), measured with `tools/stack_probe`:

| Catalog | Catalog keeps | Coffee-order card |
|---|---|---|
| 0.1.0 snippet | 225 KB | does not start: about 90 KB left |
| `load(false)` | 150 KB | runs out of memory |
| `load(true)` (compact) | 16.5 KB | renders (79 KB); 172 KB left, with a 48 KB task stack allocated |

The example's `sdkconfig.defaults.esp32c3` and `sdkconfig.defaults.esp32` turn
the compact catalog on. Deep cards still do not fit: the probe's 25-level
stress card ran out of memory on the ESP32-C3 even with the compact catalog.
Processing a message briefly needs far more heap than the result keeps:
`make heap-report` on the host shows the coffee-order card peaking at 57 KB
while it is processed (19 KB kept), the 25-level card at 121 KB (42 KB kept)
and the 50-level card at 219 KB (74 KB kept). The catalog form does not
change these peaks.

### Stack

Peak stack of a fresh task, measured on the ESP32-S3 with the catalog parsed
into RAM:

| What | Peak stack |
|---|---|
| Load the basic catalog | 6.9 KB |
| A typical card (`13_coffee-order`): process the messages, build and draw the widgets | 9.3 KB |
| A 25-level component tree: process / build and draw | 14.0 KB / 20.3 KB |
| A 50-level tree, the default limit: process / build and draw | 23.6 KB / 35.7 KB |
| Typing into a field of the 50-level card (binding, checks, re-resolution) | 35.2 KB |

The tree rows also nest the data model 46 levels and function calls 5 levels,
the deepest a message may go. Building the widget tree costs about 600 bytes
per level of component nesting, so tree depth sets the requirement. On the
ESP32-C3 (RISC-V) with the compact catalog, loading the catalog took 6.6 KB
and the typical card 8.5 KB.

Run everything that touches A2UI on one task: processing messages, building
the `Surface`, and `lv_timer_handler()`, whose event handlers write bindings
and re-resolve nodes. If an agent you do not control can send messages, give
that task about **40 KB**, or lower the component tree limit in both places
it is checked:

```cpp
a2ui::ValidationConfig validation = a2ui::ValidationConfig::strict();
validation.max_depth = 20;                  // reject deeper messages
options.validation_config = validation;     // a2ui::MessageProcessorOptions

config.resolver_options.max_depth = 20;     // a2ui_lvgl::SurfaceConfig
```

From the two measurements, a limit of 20 needs about 18 KB; 24 KB leaves a
margin. The limits on JSON nesting (64) and on message nesting (50) are fixed,
and are already included in the figures above.

## Building outside ESP-IDF

Use the root `CMakeLists.txt` (see the README), or compile the sources
directly:

- `components/a2ui_core/src/**/*.cpp`, include path `components/a2ui_core/include`
- `components/a2ui_lvgl/src/**/*.cpp`, include path `components/a2ui_lvgl/include`

The WebSocket transport in `examples/agent_surface/components/a2ui_transport_ws` is ESP-IDF
code, apart from `src/agent_session.cpp`, which needs only `a2ui_core` and a
function that sends a text frame. On another platform, feed it from your own
WebSocket client as `test/e2e/agent_e2e.cpp` does (see
[transport.md](transport.md)).

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
(`third_party/a2ui/blueprints/modules/a2ui_framework_adapter.blueprint.md`) and
use `components/a2ui_lvgl` as the worked example: `Surface` owns a
`NodeResolver`, and each resolved `ComponentNode` drives one widget.
