# Security

## Reporting a vulnerability

Please do not open a public issue for a security problem. Use
[private vulnerability reporting](https://github.com/faluciano/a2ui-lvgl/security/advisories/new)
on GitHub. You should get a reply within a week.

Only the latest release is supported with fixes.

## What counts

The renderer parses messages from an agent, and those messages may be shaped
by a language model or by an attacker who controls the connection. Treat
every message as untrusted input. These are security bugs:

- Memory errors (out-of-bounds access, use after free) reachable from a
  message.
- A crash or abort reachable from a message.
- A small message that makes the device hang or exhaust its memory, beyond
  the known limit below.
- A way for a message to read or change state outside its own surface.

## Known limits

- A regular expression with an unanchored lookahead, such as `(?=.*x)`, takes
  time proportional to the square of the input length. Inputs are capped at
  4,096 characters, which bounds the cost.
- The library does not limit how many surfaces or components an agent
  creates. Cap message size and rate in your transport.
- Recursion is bounded, but a message at the default depth limits needs
  about 36 KB of stack on the task that processes and renders it. A smaller
  stack overflows. Size the task, or lower the limits, as described in
  [docs/porting.md](docs/porting.md#stack). An overflow with a correctly sized
  stack is a security bug.

## What the library does not do

It has no transport and no authentication. Use TLS and authenticate the agent
in your own code. `Image`, `Video` and `AudioPlayer` URLs are checked against
the catalog's URL rules, but fetching is done by your image loader, which
decides which hosts to trust.
