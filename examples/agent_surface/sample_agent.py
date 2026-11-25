#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["websockets>=13,<16"]
# ///
# Copyright 2026 The a2ui-lvgl Authors
# SPDX-License-Identifier: Apache-2.0
"""A sample A2UI v1.0 agent for the agent_surface example, over a WebSocket.

    uv run examples/agent_surface/sample_agent.py [--port 8765] [--surface FILE]

It sends one surface (by default the upstream coffee-order example, from
examples/basic_surface/main/coffee_order.json) and answers the user's
actions by updating the data model: "Add to cart" adds the next item from a
small menu and recomputes the totals, "Purchase" empties the cart. Any other
file of upstream example messages works too; its actions are only logged.

The wire format is described in docs/transport.md. In short: every frame we
send is one A2UI message; every frame we get is {"message"?, "metadata"?},
and the first one on each connection carries the renderer's capabilities and,
after a reconnect, the data model the user last saw, which we restore.

No language model is involved. respond() is where one would go.
"""

from __future__ import annotations

import argparse
import asyncio
import copy
import json
import logging
import socket
import sys
from pathlib import Path
from typing import Any

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

HERE = Path(__file__).resolve().parent
DEFAULT_SURFACE = HERE.parent / "basic_surface" / "main" / "coffee_order.json"
BASIC_CATALOG = "https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json"
VERSION = "1.0"

# What "Add to cart" adds, in turn.
MENU = [
    {"name": "Cold Brew", "size": "Tall", "price": 4.95},
    {"name": "Blueberry Muffin", "size": "Warmed", "price": 3.75},
    {"name": "Matcha Latte", "size": "Grande, Oat Milk", "price": 5.85},
]
TAX_RATE = 0.09

log = logging.getLogger("sample_agent")

Json = dict[str, Any]


def load_messages(path: Path) -> list[Json]:
    """Reads an upstream example ({"messages": [...]}) or a bare message list."""
    data = json.loads(path.read_text(encoding="utf-8"))
    messages = data["messages"] if isinstance(data, dict) else data
    if not messages or "createSurface" not in messages[0]:
        raise SystemExit(f"{path}: the first message must be createSurface")
    return messages


class Agent:
    """One agent per connection. Keeps the surface's data model as it last sent it."""

    def __init__(self, messages: list[Json]) -> None:
        self.messages = messages
        self.surface_id: str = messages[0]["createSurface"]["surfaceId"]
        self.model: Json = {}
        self.added = 0  # Items added so far; picks the next one from MENU.
        for m in messages:
            update = m.get("updateDataModel")
            if update and update["surfaceId"] == self.surface_id and update.get("path", "/") == "/":
                self.model = copy.deepcopy(update["value"])

    def start(self, restored: Json | None) -> list[Json]:
        """The surface, with the restored data model when the renderer sent one."""
        if restored is not None:
            self.model = copy.deepcopy(restored)
        out = []
        for m in self.messages:
            update = m.get("updateDataModel")
            if update and update["surfaceId"] == self.surface_id and update.get("path", "/") == "/":
                m = {"version": VERSION, "updateDataModel": {"surfaceId": self.surface_id, "value": self.model}}
            out.append(m)
        return out

    def respond(self, action: Json, model: Json | None) -> list[Json]:
        """Decides what to send back for one user action.

        `model` is the surface's data model as the renderer had it when the user
        acted (the surface has sendDataModel), or None.

        This is where a language model would plug in: give it the catalog, the
        surface's components, the action and `model`, ask for A2UI messages,
        validate them against the v1.0 schemas, and return them. A rule per
        action keeps this sample predictable.
        """
        if model is not None:
            self.model = copy.deepcopy(model)
        name = action.get("name")
        if name == "add_to_cart" and isinstance(self.model.get("items"), list):
            items = self.model["items"] + [MENU[self.added % len(MENU)]]
            self.added += 1
            return self.update({"items": items, **totals(items)})
        if name == "purchase" and isinstance(self.model.get("items"), list):
            count = len(self.model["items"])
            return self.update({"storeName": f"Ordered {count} item{'s' if count != 1 else ''}. Thank you!",
                                "items": [], **totals([])})
        log.info("no reply to action %r", name)
        return []

    def update(self, values: Json) -> list[Json]:
        """One updateDataModel per top-level key, applied to our copy too."""
        out = []
        for key, value in values.items():
            self.model[key] = value
            out.append({"version": VERSION,
                        "updateDataModel": {"surfaceId": self.surface_id, "path": f"/{key}", "value": value}})
        return out


def totals(items: list[Json]) -> Json:
    subtotal = round(sum(float(i.get("price", 0)) for i in items), 2)
    tax = round(subtotal * TAX_RATE, 2)
    return {"subtotal": subtotal, "tax": tax, "total": round(subtotal + tax, 2)}


async def send_all(ws: ServerConnection, messages: list[Json]) -> None:
    for m in messages:
        await ws.send(json.dumps(m, separators=(",", ":")))


async def handle(ws: ServerConnection, messages: list[Json]) -> None:
    peer = "%s:%s" % ws.remote_address[:2]
    log.info("%s connected", peer)
    agent = Agent(messages)
    try:
        # The first frame starts the session: capabilities, and what the user saw last.
        start = json.loads(await asyncio.wait_for(ws.recv(), timeout=10))
        metadata = start.get("metadata", {})
        catalogs = metadata.get("a2uiRendererCapabilities", {}).get(VERSION, {}).get("supportedCatalogIds", [])
        if BASIC_CATALOG not in catalogs:
            log.warning("%s does not offer the v1.0 basic catalog (%s); closing", peer, catalogs)
            await ws.close(1008, "A2UI v1.0 basic catalog required")
            return
        restored = metadata.get("a2uiRendererDataModel", {}).get("surfaces", {}).get(agent.surface_id)
        if restored is not None:
            log.info("%s restoring the data model of '%s'", peer, agent.surface_id)
        await send_all(ws, agent.start(restored))

        async for raw in ws:
            frame = json.loads(raw)
            message = frame.get("message", {})
            surfaces = frame.get("metadata", {}).get("a2uiRendererDataModel", {}).get("surfaces", {})
            if "action" in message:
                action = message["action"]
                log.info("%s action %s from %s", peer, action.get("name"), action.get("sourceComponentId"))
                await send_all(ws, agent.respond(action, surfaces.get(action.get("surfaceId"))))
            elif "error" in message:
                log.warning("%s reports an error: %s", peer, json.dumps(message["error"]))
            elif "callAgentFunction" in message:
                call = message["callAgentFunction"]
                await send_all(ws, [{"version": VERSION, "agentFunctionResponse": {
                    "functionCallId": call["functionCallId"],
                    "error": {"code": "FUNCTION_NOT_FOUND",
                              "message": f"This agent has no function {call.get('call')!r}."}}}])
            else:
                log.info("%s sent %s", peer, raw[:200])
    except (ConnectionClosed, asyncio.TimeoutError):
        pass
    except (json.JSONDecodeError, KeyError, TypeError) as e:
        log.warning("%s sent a malformed frame (%s); closing", peer, e)
        await ws.close(1007, "malformed frame")
    log.info("%s disconnected", peer)


def lan_address() -> str:
    """The address other devices on the network reach this machine at (best effort)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("192.0.2.1", 9))  # Not sent anywhere; picks the outgoing interface.
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all)")
    parser.add_argument("--port", type=int, default=8765, help="port (default 8765; 0 picks a free one)")
    parser.add_argument("--surface", type=Path, default=DEFAULT_SURFACE,
                        help="A2UI v1.0 messages to send (an upstream example file)")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stderr)

    messages = load_messages(args.surface)
    async with serve(lambda ws: handle(ws, messages), args.host, args.port) as server:
        port = server.sockets[0].getsockname()[1]
        host = lan_address() if args.host == "0.0.0.0" else args.host
        print(f"listening on ws://{host}:{port}", flush=True)
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
