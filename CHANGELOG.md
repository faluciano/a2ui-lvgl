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
