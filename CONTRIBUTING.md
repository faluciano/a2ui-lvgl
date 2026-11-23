# Contributing

Thanks for helping. The most useful contributions right now are:

- **Board reports.** Flash the example, then tell us the board, the display,
  free heap and whether it drew correctly. So far it has run on one ESP32-P4
  board, so reports from other chips and displays help most.
- **Bug reports** with the A2UI messages that reproduce them.
  `build/host/render_sample messages.json out.png` shows most rendering bugs
  without a device.
- **Pull requests** for anything on the README roadmap, or a fix.

For a larger change, open an issue first so we can agree on the approach.

## Set up

You need `git`, `make`, a C++17 compiler (GCC or Clang) and
[uv](https://docs.astral.sh/uv/).

```sh
git clone --recurse-submodules --shallow-submodules https://github.com/faluciano/a2ui-lvgl.git
cd a2ui-lvgl
make test
```

`make test` takes a few minutes the first time, because it builds LVGL. It
runs the unit tests, the upstream conformance suites and the adapter tests
with AddressSanitizer and UndefinedBehaviorSanitizer.

To check the device build you also need ESP-IDF 6.0:

```sh
tools/build_examples.sh            # every supported chip
tools/build_examples.sh esp32s3    # one chip
```

## Before you open a pull request

1. `make test` passes with no new entries in `test/known_failures.txt`.
2. The change compiles for the device with no warnings
   (`tools/build_examples.sh`). CI checks this too.
3. New behaviour has a test. Protocol behaviour belongs in `test/unit/`;
   widget behaviour in `test/lvgl/`.
4. If you changed how something looks, run `make renders` and attach a before
   and after image.

## Code rules

The code runs on microcontrollers, which sets a few rules:

- **C++17, no exceptions, no RTTI.** Report failures with `a2ui::Result<T>`
  and `a2ui::Status` (`a2ui/error.h`). No `throw`, `try`, `dynamic_cast` or
  `typeid`.
- **One thread.** Nothing in the libraries locks or spawns threads.
- **Bounded recursion and input sizes.** Anything that walks input from the
  agent needs a depth or size limit, because that input is untrusted.
- **No platform headers in `components/`.** ESP-IDF and FreeRTOS code belongs
  in `examples/`.
- **Fixed-width integers need casts in format strings.** `uint32_t` is
  `unsigned long` on the ESP32 compilers.
- Two-space indent, lines up to about 110 columns, `PascalCase` types and
  `snake_case` functions. Match the file you are editing.
- Every source file starts with the copyright and SPDX lines used elsewhere.

Follow the upstream A2UI specification and blueprints in `third_party/a2ui`
when behaviour is in question. If this renderer and the specification
disagree, that is a bug here.

## Generated files

Do not edit these by hand. Each names its generator in its first lines.

| File | Generator |
|---|---|
| `components/a2ui_core/src/schema/spec_documents_data.cpp` | `tools/embed_spec_documents.py` |
| `components/a2ui_core/src/locale/locale_tables.cpp` | `tools/gen_locale_tables.py` |
| `components/a2ui_core/src/common/xid_tables.inc` | `tools/gen_xid_tables.py` |
| `test/unit/locale_intl_golden.inc` | `tools/gen_intl_golden.mjs` |

After updating the `third_party/a2ui` submodule, rerun
`tools/embed_spec_documents.py` and `make test`.

## Licensing

By contributing you agree that your contribution is licensed under the
Apache License 2.0, the license of this project. No separate agreement is
needed.

## Conduct

This project follows the [code of conduct](CODE_OF_CONDUCT.md).
