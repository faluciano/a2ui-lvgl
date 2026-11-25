# Stack and heap probe

An ESP-IDF project that measures how much stack and internal RAM the A2UI
libraries use. It produced the figures in
[docs/porting.md](../../docs/porting.md#memory-on-esp-idf).

Each scenario runs in its own FreeRTOS task, 128 KB on chips with PSRAM and
48 KB without. When the task finishes, the probe prints the peak stack it
used, the heap the scenario kept (free heap before minus after) and the free
internal heap:

| Scenario | What it does |
|---|---|
| `catalog` | Loads the basic catalog with `basic_catalog::load()` and creates the `MessageProcessor` |
| `coffee` | Processes and renders the coffee order card from `examples/basic_surface` |
| `s25-proc`, `s25-rend` | Processes, then renders, a 25-level component tree |
| `s50-proc`, `s50-rend` | The same at the default depth limits: a 50-level tree, a data model nested to the message depth limit, and a check nested to the function call limit |
| `input` | Types three values into the 50-level card's TextField |

The stress messages are `main/stress25.json` and `main/stress50.json`, made by
`gen_stress.py` (its docstring has the commands). Without PSRAM the probe
skips them: they do not fit next to the coffee card, and running out of memory
aborts.

The catalog is compact when `CONFIG_A2UI_COMPACT_CATALOG` is set, as
`sdkconfig.defaults.esp32c3` does. To compare with the loading code of 0.1.0,
choose "Catalog::from_json(*spec::parsed()), as in 0.1.0" in menuconfig,
"A2UI stack probe".

## Run it in QEMU

Install Espressif's QEMU once, then build and run:

```sh
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa qemu-riscv32
. "$IDF_PATH/export.sh"
cd tools/stack_probe
idf.py set-target esp32s3
idf.py build qemu monitor
```

Quit the monitor with `Ctrl+]` once it prints `PROBE-DONE`. QEMU emulates the
ESP32-S3 with octal PSRAM and the ESP32-C3, which has no PSRAM.

## Run it on a board

```sh
idf.py set-target esp32p4      # or esp32s3
idf.py -p PORT flash monitor
```

The target's `sdkconfig.defaults.<target>` turns on PSRAM. No display is
needed.

## Output

ESP32-S3 in QEMU, ESP-IDF v6.0.3, catalog parsed into RAM:

```
PROBE-START internal heap free 381115, Json catalog
PROBE-STACK catalog  used   6904 of 131072 bytes; heap kept +149608; internal heap free 249671, min 217640
PROBE-STACK coffee   used   9336 of 131072 bytes; heap kept  +78148; internal heap free 249671, min 217640
PROBE-STACK s25-proc used  14044 of 131072 bytes; heap kept  +33344; internal heap free 249671, min 217640
PROBE-STACK s25-rend used  20268 of 131072 bytes; heap kept +112580; internal heap free 249671, min 217640
PROBE-STACK s50-proc used  23644 of 131072 bytes; heap kept  +58864; internal heap free 249671, min 217640
PROBE-STACK s50-rend used  35660 of 131072 bytes; heap kept +242900; internal heap free 249671, min 217640
PROBE-STACK input    used  35244 of 131072 bytes; heap kept    +848; internal heap free 249671, min 217640
PROBE-DONE ok
```

ESP32-C3 in QEMU, compact catalog:

```
PROBE-START internal heap free 316456, compact catalog
PROBE-STACK catalog  used   6644 of 49152 bytes; heap kept  +16536; internal heap free 250576, min 249476
PROBE-STACK coffee   used   8524 of 49152 bytes; heap kept  +78568; internal heap free 172008, min 171096
PROBE-SKIP stress scenarios: no PSRAM
PROBE-DONE ok
```

With the 0.1.0 loading code the ESP32-C3 kept 225468 bytes for the catalog
and could not start the coffee scenario; with `basic_catalog::load(false)` it
kept 149952 bytes and ran out of memory rendering the card.

The free internal heap is read inside each task, while its stack is still
allocated; the minimum is the lowest point since boot. On the ESP32-S3 it
stays flat because, with the probe's settings, the libraries' own
allocations go to PSRAM (see below).

`PROBE-ERROR` lines name a scenario that failed. The probe stops there.

The probe sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` so that the 128 KB
task stacks fit, so its heap figures show where allocations go with that
setting, not with the example's. Run it after changing the libraries'
recursion, or to check the figures on new hardware.
