# Changelog

This project follows [Semantic Versioning](https://semver.org). Until 1.0.0,
minor versions may change the API.

## Unreleased

Tracks upstream A2UI at c26be2dd (was 58379ad4).

- `MessageProcessor::add_catalog()` adds a catalog after construction
  (upstream #3304). Surfaces created afterwards can use it, renderer
  function calls resolve it and `get_renderer_capabilities()` lists it.
  Surfaces created earlier keep the catalogs they were created with, so a
  component on one of them that names the new catalog fails with a catalog
  error. Adding an id the processor already holds is an error.
- A component's `catalogId` now resolves only against the catalogs its
  surface holds, as upstream does.
- A property declared as an inline union of a literal type with
  `DataBinding` or `FunctionCall` (for example
  `oneOf: [{type: boolean}, DataBinding, FunctionCall]`) now coerces to that
  type, as a `DynamicBoolean` does. Before, a bound ValidationResult came
  back as an object instead of `false` (upstream #4251).
- The conformance harness supports `addCatalogPaths` steps and finds the
  v0.9.1 fixtures at their new upstream location.
- Long `Text` in a `Row` wraps. LVGL flex items cannot shrink, so after each
  layout pass a Row narrows its text children when they overflow, in
  proportion to their natural width and never below their longest word.
- `Icon` draws `svgPath` names: the path (M, L, H, V, C, S, Q, T, A, Z) is
  filled into an antialiased alpha mask on the 24 x 24 view box and tinted
  with the text colour. No LVGL vector backend is needed. New
  `Theme::icon_size` (default 24).
- `DateTimeInput` enforces `min` and `max` in the picker: out-of-range days
  are disabled, the month arrows stop at the range, the time rollers snap
  back into it, and OK writes a value inside it.
- `a2ui_core`: the JSON Schema `format`s `date`, `time` and `date-time`
  now assert, as ISO 8601 dates, times of day and the two joined by `T`.
  The basic catalog tells `DateTimeInput` `min`/`max` values apart with a
  `oneOf` of these formats; as annotations all three matched, so every
  component with a literal `min` or `max` was rejected as "matches more than
  one schema". Other formats are still not checked.
- docs/adapter.md shows how to add bold and italic fonts to the theme.
- `make renders` also renders the adapter fixtures in `test/lvgl/renders`.
- `tools/simulator`: an SDL desktop simulator (`make simulator`). It shows
  A2UI v1.0 surfaces from a message file in a window you can click and type
  in, prints every message for the agent to stdout as JSON lines, and with
  `--stdin` reads further agent messages from stdin, one per line. It has
  `--size` and `--dark` like `render_sample`, plus `--exit-after`, `--click`,
  `--type` and `--screenshot` for scripted runs without a display
  (`SDL_VIDEODRIVER=dummy`). It needs SDL2; `make` and `make test` do not.
- `examples/agent_surface`: joins Wi-Fi (through `esp_wifi_remote` on the
  ESP32-P4), connects to an agent over a WebSocket and draws its surface.
  Built for the ESP32-S3 and ESP32-P4; not yet run on a board.
- `a2ui_transport_ws`, the example's WebSocket transport component, on
  `esp_websocket_client`. It queues what arrives for the LVGL task, sends the
  user's actions back with the data model when a surface asks for it, and
  reconnects. Each connection is a new session: the old surfaces are deleted
  and their data model is offered to the agent. It lives in the example, not
  in `components/`, and is not on the ESP Component Registry. See
  [docs/transport.md](docs/transport.md).
- `examples/agent_surface/sample_agent.py`: a WebSocket agent in Python, run
  with `uv run`. It sends the upstream coffee-order example and answers "Add to
  cart" and "Purchase" with data model updates.
- `make e2e`: a host build of the transport's session code connects to the
  sample agent, renders, presses buttons, reconnects, and checks each step.
- **Compact basic catalog for chips without PSRAM.** `Catalog::from_packed()`
  loads a catalog from a `PackedDocument`: the JSON document flattened into
  constant tables that stay in flash (`a2ui/schema_node.h`). The validator
  reads the schemas in place, so only the component and function summaries
  use the heap. `spec::packed()` has the basic catalog and the common types in
  this form, generated with the text by `tools/embed_spec_documents.py`. On
  the ESP32-C3 (QEMU) loading the basic catalog keeps 16.5 KB of heap instead
  of 225 KB, and the coffee-order card now renders with about 170 KB to
  spare. The packed tables add 44 KB of flash. Validation gives the same
  results: `make test` runs the core and adapter conformance suites a second
  time with every catalog packed, and they pass (672 and 36).
- `basic_catalog::load()` loads the basic catalog and binds its functions.
  It is compact when `CONFIG_A2UI_COMPACT_CATALOG` is set (new Kconfig menu
  "A2UI core"; off by default) or with `load(true)`. The example turns it on
  for the ESP32 and the ESP32-C3.
- `Catalog::from_json(Json&&)` takes the document instead of copying it.
  `basic_catalog::load(false)` parses the catalog into a temporary and moves
  it in, so nothing keeps a second copy: 150 KB instead of 225 KB on the
  ESP32-C3. That is still too much for the coffee-order card there.
- **API changes.** Schemas are read through `a2ui::SchemaNode`, a view of a
  `Json` value or of a packed node; a `const Json&` converts to one.
  `validate_schema()` and `coerce_validation_results()` take a `SchemaNode`,
  and `SchemaRegistry::PointerResolver` returns one (a resolver returning
  `const Json*` still compiles). `SchemaRegistry::resolve_pointer()` returns a
  `SchemaNode` instead of `const Json*`. In a compact catalog
  `ComponentApi::schema`, `FunctionApi::schema`, `args_schema`, `description`
  and `Catalog::source()` are undefined; `schema_node()`,
  `args_schema_node()` and `source_node()` work with both kinds of catalog.
- Reading schemas through `SchemaNode` costs some stack in shallow paths:
  on the ESP32-S3, loading the catalog peaks at 6.9 KB (was 5.4 KB) and the
  coffee-order card at 9.3 KB (was 8.6 KB). The figures for deep messages
  are unchanged (within 0.2 KB).
- `make heap-report` counts the heap the basic catalog and a card use on the
  host, for each way of loading the catalog.
- `tools/stack_probe` also prints the heap each scenario keeps, uses 48 KB
  task stacks on chips without PSRAM so that it runs on the ESP32-C3 (and
  skips the stress cards there), and can load the catalog the 0.1.0 way for
  comparison (menuconfig "A2UI stack probe").

## 0.1.0

First public release. Runs on an ESP32-P4 board.

- `a2ui_core`: A2UI v1.0 messages, data model, catalogs with JSON Schema
  validation, expressions, the 14 basic catalog functions, node resolution,
  and function calls between renderer and agent.
- `a2ui_lvgl`: the 18 basic catalog components on LVGL 9.6, with a theme,
  custom component registry, image loader hook and accessibility attributes.
- `examples/basic_surface` for ESP32, ESP32-S3, ESP32-C3 and ESP32-P4.
- Host test suite running the upstream A2UI conformance cases.

From the first hardware report (ESP32-P4):

- Shared ownership now uses `a2ui::Ref<T>` (`a2ui/ref.h`) instead of
  `std::shared_ptr`. On the ESP32-S2 and the RISC-V chips, ESP-IDF's
  libstdc++ gave every `std::shared_ptr` a FreeRTOS mutex in internal RAM;
  showing one typical card created 422 of them (about 40 KB), and one card
  could exhaust internal RAM. The same card now creates one. **API change:**
  `NodePtr`, `CatalogPtr`, `spec::parsed()`, `SchemaRegistry::add()` and
  `MessageProcessorOptions::adapters` use `a2ui::Ref`. Code that uses these
  aliases or `auto` compiles unchanged. `ImageLoader::load()` and
  `Surface::resource()` keep `std::shared_ptr<void>`.
- `make test` fails if `std::shared_ptr` comes back into `components/`.
- Measured memory and stack figures in the README and in
  [docs/porting.md](docs/porting.md#memory-on-esp-idf), with how to keep the
  libraries in PSRAM and how much stack a task needs. The earlier README
  estimates were too low.
- `tools/stack_probe`: an ESP-IDF project that measures those stack figures
  in QEMU or on a board. CI builds it with the example.
