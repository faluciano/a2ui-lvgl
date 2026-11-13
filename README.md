<img src="assets/icon.svg" width="64" height="64" alt="">

# a2ui-lgvl

An [A2UI](https://github.com/a2ui-project/a2ui) v1.0 renderer for
[LVGL](https://lvgl.io), aimed at the Espressif ESP32 family (ESP32, ESP32-S3,
ESP32-C3, ESP32-P4). It is written in C++17 and builds without exceptions or
RTTI. It follows the upstream A2UI renderer blueprints.

**Status: work in progress.** The protocol core is being built up against the
upstream conformance suites. The LVGL adapter is still a placeholder that
shows component text in a column of labels.

## Layout

| Path | Contents |
| --- | --- |
| `components/a2ui_core` | Protocol core: JSON, data model, expressions, catalogs. No UI dependency. |
| `components/a2ui_lvgl` | LVGL adapter (placeholder for now). Depends on `a2ui_core` and `lvgl`. |
| `examples/basic_surface` | ESP-IDF app that renders an embedded A2UI sample with `esp_lvgl_port`. |
| `test/unit`, `test/harness` | Host unit tests and the conformance harness. |
| `test/lvgl` | Host `lv_conf.h` and an LVGL smoke test. |
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
make -j4          # libraries, unit tests, conformance runner, LVGL smoke test
make test         # unit tests + conformance suites + LVGL smoke test
make conformance SUITE=data_model   # one suite
make lvgl-smoke   # LVGL only
```

Host builds use ASan and UBSan. The libraries are compiled with
`-fno-exceptions -fno-rtti`, as they are on device. The core library and the
conformance harness do not depend on LVGL. If `third_party/lvgl` is not
checked out, `make test` skips the LVGL smoke test.

## ESP-IDF example

Requires ESP-IDF 5.4 or later (tested with v6.0.3). LVGL, `esp_lvgl_port` and
panel drivers come from the component registry.

```sh
cd examples/basic_surface
idf.py set-target esp32s3
idf.py menuconfig   # "A2UI basic surface example": display, controller, pins
idf.py build flash monitor
```

The default display is an SPI ST7789 panel driven through `esp_lcd`. ILI9341
is the other option. Choose "Headless" to render into RAM with a dummy flush,
which runs on any board. Each target's PSRAM setting is in
`sdkconfig.defaults.<target>`.
