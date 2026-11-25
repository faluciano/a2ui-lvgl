# WebSocket transport

`examples/agent_surface` connects a device to an A2UI v1.0 agent over a
WebSocket and draws what the agent sends. It has three parts:

| Path | What |
|---|---|
| `examples/agent_surface/components/a2ui_transport_ws` | The transport, an ESP-IDF component on Espressif's `esp_websocket_client`. |
| `examples/agent_surface/main` | The app: Wi-Fi, the display, and the surface views. |
| `examples/agent_surface/sample_agent.py` | An agent to run on your computer. |

The transport is optional: the libraries take and return JSON text, and any
transport works. It lives in the example, not in `components/`, because the
libraries stay free of ESP-IDF and FreeRTOS code.

**Status.** The example builds with ESP-IDF v6.0.3, and the host end-to-end
test (`make e2e`) runs the transport's session code against the sample agent.
It has not run on a board yet; please
[report what you find](https://github.com/faluciano/a2ui-lvgl/issues).

## Try it

You need [uv](https://docs.astral.sh/uv/) on the computer that runs the agent.

1. Start the sample agent. It prints the address the device should use:

   ```sh
   uv run examples/agent_surface/sample_agent.py
   # listening on ws://192.168.1.20:8765
   ```

2. Build and flash the example:

   ```sh
   cd examples/agent_surface
   idf.py set-target esp32s3      # or esp32p4
   idf.py menuconfig              # "A2UI agent surface example": Wi-Fi, agent URI, display
   idf.py build flash monitor
   ```

The device joins Wi-Fi, connects to the agent and shows the coffee-order card.
"Add to cart" adds an item and "Purchase" empties the cart; the agent makes
both changes with `updateDataModel`. Stop the agent and start it again, and
the device reconnects and shows the cart as it was.

On the ESP32-P4, which has no radio, Wi-Fi goes through the companion chip
with `esp_wifi_remote` and `esp_hosted` (an ESP32-C6 on the
ESP32-P4-Function-EV-Board). Configure it under "Wi-Fi Remote" in menuconfig
if your board differs.

To try it without a board:

```sh
make e2e
```

That starts the sample agent on a local port, connects to it with a host build
of the session code, renders on a headless LVGL display, presses "Add to cart",
drops and reopens the connection, presses "Purchase", and checks what arrives
and what is drawn at each step.

The sample agent answers with fixed rules. `Agent.respond()` in
`sample_agent.py` is where a language model would go: give it the catalog,
the surface, the action and the data model, and send the A2UI messages it
writes once they validate.

## Use it in your app

Copy `examples/agent_surface/components/a2ui_transport_ws` into your
project's `components/` directory, or add it as a git dependency in
`main/idf_component.yml`:

```yaml
dependencies:
  a2ui_transport_ws:
    git: https://github.com/faluciano/a2ui-lvgl.git
    path: examples/agent_surface/components/a2ui_transport_ws
    version: main
```

It brings in `a2ui_core` and `esp_websocket_client`.

Then, with the LVGL lock held:

```cpp
#include "a2ui_transport_ws/ws_transport.h"

a2ui::MessageProcessorOptions options;
options.forward_actions = true;  // send the user's actions to the agent
auto* processor = new a2ui::MessageProcessor({catalog}, std::move(options));

a2ui_transport_ws::WsTransportConfig config;
config.uri = "ws://192.168.1.20:8765";
auto* transport = new a2ui_transport_ws::WsTransport(*processor, config);
transport->start();

// Hand what arrived to the processor, on the LVGL task.
lv_timer_create([](lv_timer_t* t) {
  static_cast<a2ui_transport_ws::WsTransport*>(lv_timer_get_user_data(t))->poll();
}, 20, transport);
```

The transport becomes the processor's outbound listener; do not set one in
`MessageProcessorOptions`. Surfaces come and go with the agent, so create the
`a2ui_lvgl::Surface` views when a surface appears and drop them once
`mounted()` is false. `examples/agent_surface/main/main.cpp` does this in
`sync_view()`.

`WsTransportConfig` sets the reconnect delay (5 s), the largest message
accepted from the agent (64 KB), the queue length (16 messages), how long a
send may block (1 s), extra handshake headers and, for `wss://`, the server
certificate.

### Threads

| Task | Does |
|---|---|
| `esp_websocket_client` task | Reads frames, joins fragments, and queues complete text messages and connect/disconnect events. Never touches A2UI. |
| The LVGL task, in `poll()` | Takes events off the queue and gives them to the session, which runs the processor. |
| The LVGL task, in the outbound listener | Sends each message for the agent with `esp_websocket_client_send_text`, which may block for up to the send timeout. |

`poll()` handles at most 8 events per call (an argument) so that a burst from
the agent does not stall the display. If the queue stays full for a second,
or a message is larger than the limit, the message is dropped. A lost message
would leave the device out of step with the agent, so the transport then
closes the connection and starts a new session (see below).

The LVGL task runs everything A2UI, so give it the stack that
[docs/porting.md](porting.md#stack) recommends for messages from an agent:
the example uses 40 KB.

## On the wire

A2UI v1.0 does not define a WebSocket binding, so this one follows the A2A
binding's metadata names. Every frame is a text frame holding one JSON value.

**Agent to device.** One A2UI payload per frame: a message, an array of
messages, or `{"messages": [...]}`. The processor validates it; a rejected
payload is answered with an `error` message.

```json
{"version": "1.0", "updateDataModel": {"surfaceId": "gallery-coffee-order", "path": "/total", "value": 17.06}}
```

**Device to agent.** An object with an optional `message` (a v1.0
renderer-to-agent message: `action`, `callAgentFunction`,
`rendererFunctionResponse` or `error`) and an optional `metadata`.
`renderer_to_agent.json` allows exactly two properties on a message, so the
metadata cannot go inside it.

The first frame of every connection starts the session. It has no `message`;
its metadata carries the renderer's capabilities, which apply to the whole
connection, and, after a reconnect, the data model of the surfaces that had
`sendDataModel`:

```json
{"metadata": {
  "a2uiRendererCapabilities": {"1.0": {"supportedCatalogIds": [
    "https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json"]}},
  "a2uiRendererDataModel": {"version": "1.0", "surfaces": {
    "gallery-coffee-order": {"storeName": "Sunrise Coffee", "items": ["..."]}}}
}}
```

After that, each frame carries one message. While a surface has
`sendDataModel`, `metadata.a2uiRendererDataModel` holds the data model of
every such surface, as the protocol asks:

```json
{"message": {"version": "1.0", "action": {"name": "add_to_cart",
   "surfaceId": "gallery-coffee-order", "sourceComponentId": "add_btn",
   "timestamp": "2026-11-24T10:00:00.000Z", "context": {}}},
 "metadata": {"a2uiRendererDataModel": {"version": "1.0", "surfaces": {
   "gallery-coffee-order": {"storeName": "Sunrise Coffee", "items": ["..."]}}}}}
```

## Sessions and reconnecting

Each WebSocket connection is a new session. A2UI v1.0 has no message for
resuming one, and two of its rules decide what happens:

- A `surfaceId` must be unique for the renderer's lifetime, and a
  `createSurface` for an id that still exists is an `INTEGRITY_ERROR`. An
  agent that starts over sends `createSurface` again, so the old surface has
  to go first.
- The data model the renderer sends is what the agent should treat as the
  current state ("Convergence" in the protocol's data model section).

So, in `AgentSession` (`agent_session.h`):

| When | What happens |
|---|---|
| The connection drops | Pending agent calls fail with `CANCELLED`; their answers cannot arrive. The surfaces stay on screen, out of date. Messages for the agent (a button pressed meanwhile) are dropped, not queued. |
| A connection opens | The data model of the `sendDataModel` surfaces is saved, every surface is deleted (their views unmount), and the session start frame goes out with the capabilities and the saved data model. |
| The agent answers | It creates its surfaces again. An agent that wants the user's state back reads `a2uiRendererDataModel` from the start frame, as the sample agent does. |

The screen is empty between the new connection and the agent's first
surface. An agent that keeps state per device could skip `createSurface` and
update existing surfaces instead, which the protocol allows; this transport
does not do that, because it cannot know whether the agent on the other end
is the same one.

The client reconnects by itself after an error and after a clean close by the
agent, waiting `reconnect_ms` each time.

## Limitations

- No authentication of its own. Use `wss://` (with `cert_pem` or
  `crt_bundle_attach`) and send a token in `WsTransportConfig::headers`.
- Binary frames are ignored.
- Only one agent per transport. A device that talks to several agents needs one
  transport and one processor each, or a routing layer of its own.
- The ESP-IDF part (`ws_transport.cpp`) is not covered by the host test; only
  the session logic is.
