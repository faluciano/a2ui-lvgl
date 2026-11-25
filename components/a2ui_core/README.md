# a2ui_core

The [A2UI](https://github.com/a2ui-project/a2ui) v1.0 protocol core for
embedded renderers, in C++17 with no UI dependency. It builds without
exceptions or RTTI.

It takes A2UI messages as JSON, validates them against a catalog, keeps each
surface's components and data model, resolves data bindings and function
calls, and reports the user's actions. A renderer such as
[`a2ui_lvgl`](https://github.com/faluciano/a2ui-lvgl/tree/main/components/a2ui_lvgl) turns the resolved component tree into widgets.

```cpp
#include "a2ui/basic_catalog.h"
#include "a2ui/message_processor.h"

// The basic catalog with its functions. load(true) keeps its schemas in flash.
auto catalog = a2ui::basic_catalog::load().value();

a2ui::MessageProcessorOptions options;
options.forward_actions = true;
options.outbound_listener = [](const a2ui::Json& message) {
  send_to_agent(message.dump());
};
a2ui::MessageProcessor processor({catalog}, std::move(options));
processor.process_text(json_from_agent);
```

The basic catalog takes about 17 KB of heap in its compact form (schemas in
flash; `CONFIG_A2UI_COMPACT_CATALOG` or `load(true)`) and about 150 KB parsed
into RAM, measured on an ESP32-C3. See the
[project README](https://github.com/faluciano/a2ui-lvgl#readme) for status,
memory figures and limitations.

Apache License 2.0.
