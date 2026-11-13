<img src="assets/icon.svg" width="64" height="64" alt="">

# a2ui-lgvl

An [A2UI](https://github.com/a2ui-project/a2ui) v1.0 renderer for
[LVGL](https://lvgl.io), aimed at the Espressif ESP32 family (ESP32, ESP32-S3,
ESP32-C3, ESP32-P4). It is written in C++17 and builds without exceptions or
RTTI. It follows the upstream A2UI renderer blueprints.

**Status: work in progress.** The protocol core runs the upstream core
conformance suites; the LVGL adapter renders the whole v1.0 basic catalog and
runs the framework adapter suites (`adapter_conformance.yaml`,
`accessibility.yaml`, `markdown_subset.yaml`) on a headless display.

## Layout

| Path | Contents |
| --- | --- |
| `components/a2ui_core` | Protocol core: JSON, data model, expressions, catalogs, message processing, and the `NodeResolver` that turns a surface into a live tree of resolved `ComponentNode`s. No UI dependency. |
| `components/a2ui_lvgl` | LVGL framework adapter: `Surface`, the `NodeView` dispatcher, component implementations (Form 4), the basic catalog widgets, theme, accessibility seam. Depends on `a2ui_core` and `lvgl` (v9.6+). |
| `examples/basic_surface` | ESP-IDF app that renders an embedded A2UI sample with `esp_lvgl_port`. |
| `test/unit`, `test/harness` | Host unit tests and the conformance harness. |
| `test/lvgl` | Host `lv_conf.h`, adapter unit tests, and the adapter conformance runner. |
| `tools/render_sample` | Host tool that renders an A2UI message file to a PNG through the adapter. |
| `third_party/a2ui` | Upstream A2UI (submodule): spec, catalogs, conformance suites. |
| `third_party/lvgl` | LVGL v9.6.0 (submodule), used only for host builds. |

Both components work as ESP-IDF components (`idf_component.yml`) and as plain
CMake targets (`a2ui::core`, `a2ui::lvgl`) outside ESP-IDF. For the plain
CMake build, add LVGL first so that the `lvgl` target exists.

## Host build and tests

Requirements: GCC or Clang with AddressSanitizer, GNU make, and
[uv](https://docs.astral.sh/uv/) (used to convert the YAML conformance suites).

```sh
git submodule update --init third_party/a2ui third_party/lvgl
make -j4          # libraries, unit tests, conformance runners, LVGL tests, render tool
make test         # unit tests + core conformance + LVGL adapter tests + adapter conformance
make conformance SUITE=data_model   # one core suite
make lvgl-smoke   # LVGL adapter unit tests only
make adapter-conformance CASE=modal VERBOSE=1   # framework adapter suites
make renders      # every catalogs/basic/v1/examples/*.json -> build/renders/*.png
build/host/render_sample sample.json out.png --size 320x480 --dark
```

Known conformance failures are listed with their reasons in
`test/known_failures.txt`.

Host builds use ASan and UBSan. The libraries are compiled with
`-fno-exceptions -fno-rtti`, as they are on device. The core library and the
conformance harness do not depend on LVGL. If `third_party/lvgl` is not
checked out, `make test` skips the LVGL smoke test.

## LVGL adapter

```cpp
a2ui::MessageProcessor processor({basic_catalog});
processor.process_text(json);                       // from your transport
a2ui_lvgl::SurfaceConfig config;
config.registry = &a2ui_lvgl::basic_registry();     // or your own registry
a2ui_lvgl::Surface view(*processor.model().get_surface("main"), lv_screen_active(), config);
processor.model().on_action().subscribe([](const a2ui::Json& a) { /* send to agent */ });
```

Call everything from the LVGL task (or with the LVGL lock held). `Surface`
owns a `NodeResolver`, renders its root node and tears down when destroyed,
when the surface is deleted (`deleteSurface`), or when its container is
deleted from outside. Each node is a `NodeView` keyed by instance id: a
reorder moves objects, a type change recreates them, and pending,
unknown-type or cyclic nodes render nothing (no object, no layout slot)
unless `SurfaceConfig::fallbacks` supplies a view.

Components are `ComponentInstance` objects (blueprint Form 4: `mount`,
`update`, `unmount`) created by a `ComponentRegistry` keyed by component
type and, optionally, catalog id. `register_basic_components()` fills one
with the basic catalog; replace any entry with your own widget.

Presentation comes from `a2ui_lvgl::Theme` (colours, fonts, spacing; v1.0
has no protocol theme). Leaf components and outlined containers carry a
uniform margin, Row/Column/List none; text colour reaches `Text` and `Icon`
through LVGL style inheritance; cards are outlined and transparent.

Host hooks (`SurfaceConfig::services`): an `ImageLoader` for `Image`
pixels, a text-focus hook to attach an on-screen `lv_keyboard`, a live-region
announcer, and an icon glyph mapper. Accessibility attributes (explicit
`@accessibility` over values inferred from visible text) are kept per view
and read with `a2ui_lvgl::accessibility_of(lv_obj_t*)`; LVGL itself has no
accessibility API.

Microcontroller limitations:

- `Video` and `AudioPlayer` render nothing (no media pipeline); register an
  implementation of your own to show a poster or a player.
- `Image` shows a placeholder box sized by its variant until the
  application's `ImageLoader` delivers pixels; URLs are validated with the
  catalog's media URL rules first.
- `Text` renders a Markdown subset (headings, emphasis, code, lists,
  quotes, links as text, images as alt text, raw HTML as text). Bold, italic
  and code runs need the theme to provide those fonts; the built-in
  Montserrat fonts have none, so the markers are stripped and the run uses
  the body font. Glyphs outside the fonts' ranges (emoji, stars) show as
  boxes.
- `Icon` maps names to `LV_SYMBOL_*` glyphs with ASCII stand-ins; `svgPath`
  icons draw a bullet.
- `DateTimeInput` uses `lv_calendar` and hour/minute rollers; ISO 8601
  zone designators are kept verbatim, never converted; `min`/`max` are not
  enforced by the picker.
- A Row cannot shrink its children (LVGL flex has no shrink), so long text
  in a Row does not wrap; in a Column it does.

## ESP-IDF example

Requires ESP-IDF 5.4 or later (tested with v6.0.3). LVGL, `esp_lvgl_port` and
panel drivers come from the component registry.

```sh
cd examples/basic_surface
idf.py set-target esp32s3
idf.py menuconfig   # "A2UI basic surface example": display, controller, pins
idf.py build flash monitor
```

The app renders `catalogs/basic/v1/examples/13_coffee-order.json` (embedded
at build time) and logs button actions with `ESP_LOGI`.

The default display is an SPI ST7789 panel driven through `esp_lcd`. ILI9341
is the other option. Choose "Headless" to render into RAM with a dummy flush,
which runs on any board. Each target's PSRAM setting is in
`sdkconfig.defaults.<target>`.
