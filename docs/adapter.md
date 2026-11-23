# The LVGL adapter

`a2ui_lvgl` draws A2UI surfaces with LVGL 9 widgets. It follows the upstream
framework adapter blueprint (`third_party/a2ui/blueprints/modules/a2ui_framework_adapter.blueprint.md`),
in its imperative form: each component has `mount`, `update` and `unmount`.

## Minimal use

```cpp
auto catalog = a2ui::Catalog::from_json(
    *a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1)).value();
a2ui::basic_catalog::bind_functions(*catalog);

a2ui::MessageProcessorOptions options;
options.forward_actions = true;
options.outbound_listener = [](const a2ui::Json& message) {
  send_to_agent(message.dump());
};
a2ui::MessageProcessor processor({catalog}, std::move(options));

processor.process_text(json);                       // from your transport

a2ui_lvgl::SurfaceConfig config;
config.registry = &a2ui_lvgl::basic_registry();     // or your own registry
a2ui_lvgl::Surface view(*processor.model().get_surface("main"), lv_screen_active(), config);
```

## The host loop

```cpp
for (;;) {
  processor.tick(esp_timer_get_time() / 1000);  // Agent call timeouts (default 30 s).
  lv_timer_handler();
  // processor.process_text(...) for each framed message from the transport.
}
```

Without `tick()`, agent calls never time out.

## Messages to the agent

The outbound listener receives complete renderer-to-agent messages, ready to
serialize:

| Message | When |
|---|---|
| `action` | The user triggers a component's action. Only with `forward_actions = true`. |
| `callAgentFunction` | A binding or action calls a function the device cannot run itself. |
| `rendererFunctionResponse` | The reply to the agent's `callRendererFunction`. |
| `error` | A message from the agent was rejected. |

Without an outbound listener, functions the renderer cannot run locally fail
instead of being sent to the agent.

With `forward_actions = false` (the default, as in the upstream SDKs), read
actions from `processor.model().on_action()` and wrap each one as
`{"version": "1.0", "action": ...}` yourself. That is also the place to
filter or handle an action on the device.

Failures inside one surface, such as a function that fails while a binding is
evaluated, are reported on that surface's `on_error()` and are not sent to the
agent automatically.

To learn when surfaces appear and disappear, subscribe to
`processor.model().on_surface_created()` and `on_surface_deleted()`.

## Threading and lifetime

Call everything from the LVGL task (or with the LVGL lock held). `Surface`
owns a `NodeResolver`, renders its root node and tears down when destroyed,
when the surface is deleted (`deleteSurface`), or when its container is
deleted from outside. Each node is a `NodeView` keyed by instance id: a
reorder moves objects, a type change recreates them, and pending,
unknown-type or cyclic nodes render nothing (no object, no layout slot)
unless `SurfaceConfig::fallbacks` supplies a view.

## Custom components

Components are `ComponentInstance` objects (blueprint Form 4: `mount`,
`update`, `unmount`) created by a `ComponentRegistry` keyed by component
type and, optionally, catalog id. `register_basic_components()` fills one
with the basic catalog; replace any entry with your own widget.

```cpp
a2ui_lvgl::ComponentRegistry registry = a2ui_lvgl::basic_registry();  // copy
registry.add({"Video", "", [] { return std::make_unique<MyPosterView>(); }});
config.registry = &registry;
```

`MyPosterView` derives from `a2ui_lvgl::ComponentInstance`
(`a2ui_lvgl/component.h`). The basic catalog widgets under
`components/a2ui_lvgl/src/catalog/basic/` are the reference.

## Theme

Presentation comes from `a2ui_lvgl::Theme` (colours, fonts, spacing; v1.0
has no protocol theme). Leaf components and outlined containers carry a
uniform margin, Row/Column/List none; text colour reaches `Text` and `Icon`
through LVGL style inheritance; cards are outlined and transparent.

## Host hooks

Host hooks (`SurfaceConfig::services`): an `ImageLoader` for `Image`
pixels, a text-focus hook to attach an on-screen `lv_keyboard`, a live-region
announcer, and an icon glyph mapper. Accessibility attributes (explicit
`@accessibility` over values inferred from visible text) are kept per view
and read with `a2ui_lvgl::accessibility_of(lv_obj_t*)`; LVGL itself has no
accessibility API.

## Limitations

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
