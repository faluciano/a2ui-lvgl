#!/usr/bin/env python3
"""Generate an A2UI v1.0 message list that drives every bounded recursion near its limit.

usage: gen_stress.py <out.json> <tree_depth> <data_depth> <call_depth> [surface_id]

- tree_depth: Columns nested inside each other, root first, leaf a TextField.
  Each Column also holds a Text whose formatString reads the data model.
- data_depth: objects nested inside the data model's "deep" key.
- call_depth: "not" calls wrapped around the TextField check's regex call.

The committed files were made with:
  gen_stress.py main/stress50.json 50 46 4 stress50
  gen_stress.py main/stress25.json 25 46 4 stress25
50 is the default tree limit, 46 data levels plus the message's own nesting
reach the default message depth limit (50), and 4 "not" calls plus the regex
reach the function call depth limit (5).
"""
import json
import sys

out, tree_depth, data_depth, call_depth = sys.argv[1], *map(int, sys.argv[2:5])
sid = sys.argv[5] if len(sys.argv) > 5 else "stress"

components = []
for i in range(tree_depth):
    child = f"col{i + 1}" if i + 1 < tree_depth else "field"
    components.append({"id": "root" if i == 0 else f"col{i}", "component": "Column",
                       "children": [child, f"label{i}"]})
    deep_path = "/deep" + "/n" * min(i, data_depth)
    components.append({"id": f"label{i}", "component": "Text",
                       "text": {"@call": "formatString",
                                "args": {"value": "${/form/name} ${" + deep_path + "/v}"}}})

condition = {"@call": "regex", "args": {"value": {"@path": "/form/name"}, "pattern": "^[a-z]{1,20}$"}}
for _ in range(call_depth):
    condition = {"@call": "not", "args": {"value": condition}}
components.append({"id": "field", "component": "TextField", "label": "Name",
                   "value": {"@path": "/form/name"},
                   "@checks": [{"condition": condition, "message": "letters only"}]})

deep = {"v": "leaf"}
for _ in range(data_depth):
    deep = {"n": deep, "v": "x"}

messages = [
    {"version": "1.0", "createSurface": {"surfaceId": sid,
     "catalogId": "https://a2ui.org/specification/v1_0/catalogs/basic/catalog.json"}},
    {"version": "1.0", "updateComponents": {"surfaceId": sid, "components": components}},
    {"version": "1.0", "updateDataModel": {"surfaceId": sid, "value": {"form": {"name": "abc"}, "deep": deep}}},
]
with open(out, "w") as f:
    json.dump(messages, f)
    f.write("\n")
