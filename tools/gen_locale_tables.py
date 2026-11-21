# Copyright 2026 The a2ui-lvgl Authors
# SPDX-License-Identifier: Apache-2.0
"""Generates the compiled-in CLDR tables for components/a2ui_core/src/locale.

    ~/.local/bin/uv run --no-project --with babel==2.18.0 \
        python tools/gen_locale_tables.py > components/a2ui_core/src/locale/locale_tables.cpp

Babel carries CLDR, but its merged locale data resolves some CLDR aliases to
root placeholders ("M03", "Sun") instead of the locale's own alias target, so
month and day names are resolved here from the unmerged data (see `_resolve`).

Flash layout: every string lives once in a NUL-separated pool (a string that
is the tail of another shares its bytes), name lists are deduplicated as
whole lists, plural rules are compiled to a small bytecode shared between
locales with the same rules, and currencies store only what differs from
"symbol = ISO code, 2 digits".
"""

import os
import pickle
import re
import sys
import unicodedata

import babel
from babel import Locale, localedata
from babel.core import get_cldr_version, get_global
from babel.numbers import get_currency_precision, get_currency_symbol, list_currencies

# Tag -> Babel locale. The first tag of each language is its fallback.
LOCALES = [
    ("en", "en"),
    ("en-GB", "en_GB"),
    ("de", "de"),
    ("fr", "fr"),
    ("es", "es"),
    ("it", "it"),
    ("pt", "pt"),
    ("pt-BR", "pt_BR"),
    ("nl", "nl"),
    ("sv", "sv"),
    ("ja", "ja"),
    ("zh", "zh"),
    ("ko", "ko"),
    ("ru", "ru"),
    ("pl", "pl"),
    ("ar", "ar"),
]

# CLDR numbers/minimumGroupingDigits. Babel does not expose it; Intl honours it
# (useGrouping "auto"), so "1234,5" in es but "12.345,5". Every locale not
# listed here uses 1. Values as in CLDR 48 (Node 24 / ICU 78), which added it.
MIN_GROUPING = {"es": 2, "it": 2, "pl": 2}

PLURAL_CATEGORIES = ["zero", "one", "two", "few", "many", "other"]
OPERANDS = "nivwftec"


# ---------------------------------------------------------------------------
# String pool


class Pool:
    def __init__(self):
        self.strings = {"": None}

    def add(self, s):
        self.strings.setdefault(s, None)
        return s

    def build(self):
        """Lays strings out longest first; a tail of a placed string reuses it."""
        data = bytearray(b"\0")  # Offset 0 is the empty string.
        offsets = {"": 0}
        placed = []  # (encoded, offset)
        for s in sorted(self.strings, key=lambda s: (-len(s.encode()), s)):
            if s == "":
                continue
            enc = s.encode()
            for other, off in placed:
                if other.endswith(enc):
                    offsets[s] = off + len(other) - len(enc)
                    break
            else:
                offsets[s] = len(data)
                placed.append((enc, len(data)))
                data += enc + b"\0"
        assert len(data) < 0xFFFF
        self.offsets = offsets
        self.data = bytes(data)

    def __getitem__(self, s):
        return self.offsets[s]


def c_bytes_literal(data, indent="    ", width=76):
    """Renders bytes as adjacent C string literals, breaking after each NUL."""
    lines = []
    cur = ""
    for b in data:
        if 0x20 <= b < 0x7F and chr(b) not in "\"\\?":
            piece = chr(b)
        else:
            piece = "\\%03o" % b
        cur += piece
        if b == 0 or len(cur) >= width:
            lines.append(cur)
            cur = ""
    if cur:
        lines.append(cur)
    # The final literal's implicit NUL is not part of the pool; harmless.
    return "\n".join('%s"%s"' % (indent, l) for l in lines)


# ---------------------------------------------------------------------------
# Names


def _chain(name):
    """CLDR inheritance chain of a Babel locale name, most specific first."""
    exceptions = get_global("parent_exceptions")
    chain = [name]
    while name != "root":
        name = exceptions.get(name) or (name.rsplit("_", 1)[0] if "_" in name else "root")
        chain.append(name)
    return chain


_RAW = {}


def _raw(name):
    """A locale's own CLDR data, without inheritance.

    Read from Babel's data file directly: localedata.load() caches by name
    whether or not it merged, so its unmerged view is not reliable.
    """
    if name not in _RAW:
        with open(os.path.join(localedata._dirname, name + ".dat"), "rb") as f:
            _RAW[name] = pickle.load(f)
    return _RAW[name]


def _resolve(chain, path, size):
    """Resolves months/days data at `path` with CLDR inheritance and aliases.

    Babel's merged view resolves a root alias such as stand-alone -> format
    against root's own data ("M03", "Sun") instead of re-resolving the target
    in the requesting locale, which is what CLDR specifies.
    """
    result = {}
    for name in chain:
        node = _raw(name)
        for key in path:
            node = node.get(key) if isinstance(node, dict) else None
            if node is None:
                break
        if isinstance(node, localedata.Alias):
            for k, v in _resolve(chain, node.keys, size).items():
                result.setdefault(k, v)
            break
        if isinstance(node, dict):
            for k, v in node.items():
                result.setdefault(k, v)
            if len(result) == size:
                break
    assert len(result) == size, (chain, path, result)
    return result


def _names(loc, kind, ctx, width):
    """Names for one context/width; index 0 is January / Monday (Babel order)."""
    size = 12 if kind == "months" else 7
    data = _resolve(_chain(str(loc)), (kind, ctx, width), size)
    return [data[k] for k in sorted(data)]


def month_lists(loc):
    out = []
    for ctx in ("format", "stand-alone"):
        for width in ("wide", "abbreviated", "narrow"):
            out.append(_names(loc, "months", ctx, width))
    return out


def day_lists(loc):
    out = []
    for ctx in ("format", "stand-alone"):
        for width in ("wide", "abbreviated", "narrow"):
            mon_first = _names(loc, "days", ctx, width)  # Babel: 0 = Monday.
            out.append(mon_first[6:] + mon_first[:6])  # Sunday first.
    return out


# ---------------------------------------------------------------------------
# Plural rules -> bytecode
#
#   program  := rule* 0xFF
#   rule     := category:u8 n_or:u8 and_list{n_or}
#   and_list := n_rel:u8 relation{n_rel}
#   relation := head:u8 [mod:varint] n_ranges:u8 (lo:varint hi:varint){n_ranges}
#   head     := operand(0..7, "nivwftec") | negated << 3 | has_mod << 4


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def compile_relation(text):
    m = re.fullmatch(
        r"([nivwftec])(?:\s*(?:mod|%)\s*(\d+))?\s*(not in|in|!=|=|is not|is)\s*(.+)", text.strip()
    )
    assert m, text
    operand, mod, op, ranges = m.groups()
    negated = op in ("not in", "!=", "is not")
    out = bytearray([OPERANDS.index(operand) | (negated << 3) | ((mod is not None) << 4)])
    if mod is not None:
        out += varint(int(mod))
    parts = [p.strip() for p in ranges.split(",")]
    out.append(len(parts))
    for p in parts:
        lo, _, hi = p.partition("..")
        out += varint(int(lo)) + varint(int(hi or lo))
    return bytes(out)


def compile_plural(loc):
    rules = loc.plural_form.rules
    out = bytearray()
    for cat in PLURAL_CATEGORIES[:-1]:
        if cat not in rules:
            continue
        ors = [o.strip() for o in re.split(r"\bor\b", rules[cat])]
        out += bytes([PLURAL_CATEGORIES.index(cat), len(ors)])
        for o in ors:
            ands = [a.strip() for a in re.split(r"\band\b", o)]
            out.append(len(ands))
            for a in ands:
                out += compile_relation(a)
    out.append(0xFF)
    return bytes(out)


# ---------------------------------------------------------------------------
# Currencies


def is_symbol_char(ch):
    """CLDR currencySpacing currencyMatch is [[:^S:]&[:^Z:]]; True = no space."""
    cat = unicodedata.category(ch)
    return cat[0] in "SZ"


def symbol_flags(sym):
    if not sym:
        return 3
    return (1 if is_symbol_char(sym[0]) else 0) | (2 if is_symbol_char(sym[-1]) else 0)


# ---------------------------------------------------------------------------


def main():
    pool = Pool()
    locales = []
    for tag, babel_name in LOCALES:
        loc = Locale.parse(babel_name)
        sym = loc.number_symbols["latn"]
        dec = loc.decimal_formats[None]
        assert dec.grouping == (3, 3) and dec.frac_prec == (0, 3), (tag, dec)
        assert dec.prefix == ("", "-") and dec.suffix == ("", ""), (tag, dec.prefix)
        cur = loc.currency_formats["standard"]
        assert cur.grouping == (3, 3), tag
        (pp, np), (ps, ns) = cur.prefix, cur.suffix
        rec = {
            "tag": tag,
            "decimal": sym["decimal"],
            "group": sym["group"],
            "minus": sym["minusSign"],
            "min_grouping": MIN_GROUPING.get(tag.split("-")[0], 1),
            "cur": (pp, ps, np, ns),
            "months": month_lists(loc),
            "days": day_lists(loc),
            "plural": compile_plural(loc),
            "loc": loc,
        }
        for s in (rec["decimal"], rec["group"], rec["minus"], *rec["cur"]):
            pool.add(s)
        for lst in rec["months"] + rec["days"]:
            for s in lst:
                pool.add(s)
        locales.append(rec)

    # Currencies: every ISO code in current use somewhere where some locale
    # shows a symbol other than the code, or whose default digits are not 2.
    # Anything else (historic codes, unassigned but well-formed ones such as
    # XYZ) formats with the code as its symbol and 2 digits, as Intl does.
    current = {
        entry[0]
        for entries in get_global("territory_currencies").values()
        for entry in entries
        if entry[2] is None
    }
    en = locales[0]["loc"]
    symbols = {}  # text -> index

    def symbol_index(text):
        if text not in symbols:
            symbols[text] = len(symbols)
            pool.add(text)
        return symbols[text]

    currencies = []
    overrides = {r["tag"]: [] for r in locales}
    for code in sorted(c for c in list_currencies() if c in current):
        digits = get_currency_precision(code)
        base = get_currency_symbol(code, en)
        per_locale = {r["tag"]: get_currency_symbol(code, r["loc"]) for r in locales}
        if base == code and digits == 2 and all(s == code for s in per_locale.values()):
            continue
        currencies.append((code, digits, None if base == code else symbol_index(base)))
        for tag, s in per_locale.items():
            if s != base:
                overrides[tag].append((len(currencies) - 1, symbol_index(s)))

    assert len(symbols) < 255 and len(currencies) < 0xFFFF

    # Deduplicate name lists and plural programs.
    lists12, lists7, plural_progs = [], [], []

    def intern(table, item):
        if item not in table:
            table.append(item)
        return table.index(item)

    plural_offsets = {}
    plural_blob = bytearray()
    for r in locales:
        r["month_idx"] = [intern(lists12, tuple(l)) for l in r["months"]]
        r["day_idx"] = [intern(lists7, tuple(l)) for l in r["days"]]
        if r["plural"] not in plural_offsets:
            plural_offsets[r["plural"]] = len(plural_blob)
            plural_blob += r["plural"]
        r["plural_off"] = plural_offsets[r["plural"]]

    # Override runs, shared between locales with identical lists.
    override_blob = []
    for r in locales:
        ov = overrides[r["tag"]]
        start = None
        for i in range(len(override_blob) - len(ov) + 1):
            if override_blob[i : i + len(ov)] == ov:
                start = i
                break
        if start is None:
            start = len(override_blob)
            override_blob += ov
        r["ov"] = (start, len(ov))

    # Locales whose data equals an earlier locale become aliases.
    def key(r):
        return (r["decimal"], r["group"], r["minus"], r["min_grouping"], r["cur"], tuple(r["month_idx"]),
                tuple(r["day_idx"]), r["plural_off"], r["ov"])

    unique, tags = [], []
    for r in locales:
        k = key(r)
        for i, u in enumerate(unique):
            if key(u) == k:
                tags.append((r["tag"], i))
                break
        else:
            tags.append((r["tag"], len(unique)))
            unique.append(r)
    for tag, _ in tags:
        pool.add(tag)
    pool.build()

    w = sys.stdout.write
    w("// Copyright 2026 The a2ui-lvgl Authors\n")
    w("// SPDX-License-Identifier: Apache-2.0\n//\n")
    w("// GENERATED by tools/gen_locale_tables.py from CLDR %s (Babel %s). Do not edit.\n"
      % (get_cldr_version(), babel.__version__))
    w("// Locales: %s\n\n" % ", ".join(t for t, _ in LOCALES))
    w('#include "locale_tables.h"\n\nnamespace a2ui {\nnamespace locale {\nnamespace detail {\n\n')
    w("// %d bytes.\nconst char kPool[] =\n%s;\n\n" % (len(pool.data), c_bytes_literal(pool.data)))

    w("const uint16_t kNames12[][12] = {\n")
    for l in lists12:
        w("    {%s},  // %s\n" % (", ".join(str(pool[s]) for s in l), l[0].encode("unicode_escape").decode()))
    w("};\n\n")
    w("const uint16_t kNames7[][7] = {\n")
    for l in lists7:
        w("    {%s},  // %s\n" % (", ".join(str(pool[s]) for s in l), l[0].encode("unicode_escape").decode()))
    w("};\n\n")

    w("const uint8_t kPluralCode[] = {\n")
    for i in range(0, len(plural_blob), 16):
        w("    %s,\n" % ", ".join("0x%02X" % b for b in plural_blob[i : i + 16]))
    w("};\n\n")

    sym_list = sorted(symbols, key=symbols.get)
    w("const CurrencySymbol kCurrencySymbols[] = {\n")
    for s in sym_list:
        w("    {%d, %d},  // %s\n" % (pool[s], symbol_flags(s), s.encode("unicode_escape").decode()))
    w("};\n\n")

    w("const CurrencyInfo kCurrencies[] = {\n")
    for code, digits, sym in currencies:
        w("    {{'%s', '%s', '%s'}, %d, %d},\n" % (code[0], code[1], code[2], digits,
                                                  255 if sym is None else sym))
    w("};\nconst uint16_t kCurrencyCount = %d;\n\n" % len(currencies))

    w("const CurrencyOverride kCurrencyOverrides[] = {\n")
    for cur_i, sym_i in override_blob:
        w("    {%d, %d},  // %s %s\n" % (cur_i, sym_i, currencies[cur_i][0],
                                       sym_list[sym_i].encode("unicode_escape").decode()))
    if not override_blob:
        w("    {0, 0},\n")
    w("};\n\n")

    w("const LocaleData kLocales[] = {\n")
    for r in unique:
        pp, ps, np, ns = r["cur"]
        w("    {%d, %d, %d, %d, {%d, %d, %d, %d}, {%s}, {%s}, %d, %d, %d},  // %s\n" % (
            pool[r["decimal"]], pool[r["group"]], pool[r["minus"]], r["min_grouping"],
            pool[pp], pool[ps], pool[np], pool[ns],
            ", ".join(map(str, r["month_idx"])), ", ".join(map(str, r["day_idx"])),
            r["plural_off"], r["ov"][0], r["ov"][1], r["tag"]))
    w("};\n\n")

    w("const LocaleTag kLocaleTags[] = {\n")
    for tag, i in sorted(tags, key=lambda t: t[0].lower()):
        w("    {%d, %d},  // %s\n" % (pool[tag], i, tag))
    w("};\nconst uint8_t kLocaleTagCount = %d;\n\n" % len(tags))

    w("}  // namespace detail\n}  // namespace locale\n}  // namespace a2ui\n")


if __name__ == "__main__":
    main()
