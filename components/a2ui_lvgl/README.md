# a2ui_lvgl

Renders [A2UI](https://github.com/a2ui-project/a2ui) v1.0 surfaces with
[LVGL](https://lvgl.io) 9 widgets. An AI agent describes a screen in JSON, and
the device draws it and sends the user's actions back.

It implements the 18 components of the A2UI basic catalog on top of
[`a2ui_core`](https://github.com/faluciano/a2ui-lvgl/tree/main/components/a2ui_core), and lets you replace any of them with your own
widget.

```cpp
#include "a2ui_lvgl/a2ui_lvgl.h"

// `processor` is an a2ui::MessageProcessor; see a2ui_core.
a2ui_lvgl::SurfaceConfig config;
config.registry = &a2ui_lvgl::basic_registry();
a2ui_lvgl::Surface view(*processor.model().get_surface("main"),
                        lv_screen_active(), config);
```

Call everything from the LVGL task. The example `basic_surface` is a complete
ESP-IDF app for SPI displays.

See the [project README](https://github.com/faluciano/a2ui-lvgl#readme) for
status, memory figures and limitations, and the
[adapter guide](https://github.com/faluciano/a2ui-lvgl/blob/main/docs/adapter.md)
for custom components, the theme and the host hooks.

Apache License 2.0.
