#!/usr/bin/env python3
"""Convert upstream conformance YAML suites to JSON for the C++ harness.

The C++ harness has no YAML parser (it is not needed on device), so the
suites are converted once at build time. Run through uv so PyYAML is
available without a system pip:

    uv run --with pyyaml tools/yaml2json.py <in.yaml> <out.json>
"""

import datetime
import json
import math
import sys

import yaml


def _default(value):
  # YAML timestamps would otherwise fail to serialize; keep their text form.
  if isinstance(value, (datetime.date, datetime.datetime)):
    return value.isoformat()
  raise TypeError(f"unserializable YAML value: {value!r}")


def _encode_non_finite(value):
  # JSON has no NaN or Infinity, but a few cases use YAML's `.nan`/`.inf` to
  # test values that have no JSON form. Encode them as {"$number": "NaN"}.
  if isinstance(value, float) and not math.isfinite(value):
    text = "NaN" if math.isnan(value) else ("Infinity" if value > 0 else "-Infinity")
    return {"$number": text}
  if isinstance(value, dict):
    return {k: _encode_non_finite(v) for k, v in value.items()}
  if isinstance(value, list):
    return [_encode_non_finite(v) for v in value]
  return value


def main(argv):
  if len(argv) != 3:
    sys.exit("usage: yaml2json.py <in.yaml> <out.json>")
  with open(argv[1], encoding="utf-8") as f:
    data = _encode_non_finite(yaml.safe_load(f))
  with open(argv[2], "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, allow_nan=False, default=_default)


if __name__ == "__main__":
  main(sys.argv)
