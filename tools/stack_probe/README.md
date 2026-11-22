# Stack and heap probe

An ESP-IDF project that measures how much stack and internal RAM the A2UI
libraries use. It produced the figures in
[docs/porting.md](../../docs/porting.md#memory-on-esp-idf).

Each scenario runs in its own 128 KB FreeRTOS task. When the task finishes,
the probe prints the peak stack it used and the free internal heap:

| Scenario | What it does |
|---|---|
| `catalog` | Loads the basic catalog and creates the `MessageProcessor` |
| `coffee` | Processes and renders the coffee order card from `examples/basic_surface` |
| `s25-proc`, `s25-rend` | Processes, then renders, a 25-level component tree |
| `s50-proc`, `s50-rend` | The same at the default depth limits: a 50-level tree, a data model nested to the message depth limit, and a check nested to the function call limit |
| `input` | Types three values into the 50-level card's TextField |

The stress messages are `main/stress25.json` and `main/stress50.json`, made by
`gen_stress.py` (its docstring has the commands).

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
ESP32-S3 with octal PSRAM and the ESP32-C3, which has no PSRAM and runs out
of memory partway through.

## Run it on a board

```sh
idf.py set-target esp32p4      # or esp32s3
idf.py -p PORT flash monitor
```

The target's `sdkconfig.defaults.<target>` turns on PSRAM. No display is
needed.

## Output

ESP32-S3 in QEMU, ESP-IDF v6.0.3:

```
PROBE-START internal heap free 381115
PROBE-STACK catalog  used   5436 of 131072 bytes; internal heap free 249671, min 217640
PROBE-STACK coffee   used   8620 of 131072 bytes; internal heap free 249671, min 86460
PROBE-STACK s25-proc used  14012 of 131072 bytes; internal heap free 249671, min 86460
PROBE-STACK s25-rend used  20256 of 131072 bytes; internal heap free 249671, min 86460
PROBE-STACK s50-proc used  23612 of 131072 bytes; internal heap free 249671, min 86460
PROBE-STACK s50-rend used  35456 of 131072 bytes; internal heap free 249671, min 86460
PROBE-STACK input    used  35228 of 131072 bytes; internal heap free 249671, min 86460
PROBE-DONE ok
```

The free internal heap is read inside each task, while its 128 KB stack is
still allocated. It stays flat here because, with the probe's settings, the
libraries' own allocations go to PSRAM (see below); the minimum is the lowest
point since boot.

`PROBE-ERROR` lines name a scenario that failed. The probe stops there.

The probe sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` so that the 128 KB
task stacks fit, so its heap figures show where allocations go with that
setting, not with the example's. Run it after changing the libraries'
recursion, or to check the figures on new hardware.
