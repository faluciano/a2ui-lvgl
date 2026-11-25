# Conformance

The tests run the upstream A2UI conformance suites from the `third_party/a2ui`
submodule, so the numbers below refer to that pinned commit. `make test`
reproduces them.

## Core suites

`make conformance` runs `conformance/core/*.yaml` against `a2ui_core`.

| Result | Cases | Meaning |
|---|---|---|
| Pass | 672 | |
| Fail | 0 | |
| Known failure | 1 | Listed with its reason in `test/known_failures.txt` |
| Unsupported | 197 | Not run; see below |

The **known failure** is
`test_agent_routing_cancelled_call_is_sent_again_when_its_value_is_rewritten`.
The suite gives that rule two variants and each implementation runs the one
that fits how it re-evaluates bindings. This core passes the other variant, as
the upstream Python and Swift cores do.

The **unsupported** cases are:

| Cases | Reason |
|---|---|
| 166 | They target protocol v0.8 or v0.9. This renderer implements v1.0 only. |
| 29 | They need a UI. They run in the adapter suites below instead. |
| 2 | v0.8 messages without a version header. |

One caveat on the `node_resolution` suite: 8 of its 32 cases are written for
v1.0. The other 24 are v0.9.1 fixtures, which the test harness converts to
v1.0 before running them. All 32 pass.

## Adapter suites

`make adapter-conformance` runs `adapter_conformance.yaml`,
`accessibility.yaml` and `markdown_subset.yaml` against `a2ui_lvgl` on a
headless LVGL display: 36 of 36 pass.

## Unit tests

`make unit` runs 99 tests of the core. `make lvgl-smoke` runs 107 checks of
the adapter. Formatting output is compared with golden values generated from
Node's `Intl` (`tools/gen_intl_golden.mjs`).

## How the suites run

- `tools/yaml2json.py` converts each YAML suite to JSON, because the device
  code has no YAML parser.
- `test/harness/` maps each suite action to the core API.
- Host builds use AddressSanitizer and UndefinedBehaviorSanitizer, and compile
  the libraries with `-fno-exceptions -fno-rtti` as on the device.

Run one suite or one case with:

```sh
make conformance SUITE=data_model
make conformance CASE=test_agent_routing VERBOSE=1
make adapter-conformance CASE=modal VERBOSE=1
```
