// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// The Markdown subset the Text component renders. A small parser, no
// allocation beyond the result, no HTML:
//
//   Blocks:  ATX headings (# .. ######), paragraphs (soft line breaks join
//            with a space; a trailing backslash or two spaces breaks),
//            "-", "*", "+" bullets and "1." / "1)" ordered items (nesting by
//            indentation), "> " block quotes, ``` / ~~~ fenced code, and
//            thematic breaks (---, ***, ___), which are dropped.
//   Inline:  **strong** / __strong__, *emphasis* / _emphasis_, `code`,
//            backslash escapes. Links render as their text with no target
//            ([text](url) -> text, <https://x> -> https://x); images render
//            as their alt text, never as an image; raw HTML is plain text.
//
// The Text component maps blocks and runs onto LVGL: a plain single-style
// result becomes an lv_label (the common case, cheapest), anything with
// headings, emphasis or several blocks becomes an lv_spangroup, using the
// theme's heading, bold, italic and code fonts. A font the theme does not
// provide falls back to the body font, so the markers are always stripped.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace a2ui_lvgl {
namespace markdown {

enum Style : uint8_t { kPlain = 0, kStrong = 1, kEmphasis = 2, kCode = 4 };

struct Run {
  std::string text;
  uint8_t style = kPlain;
};

struct Block {
  enum class Kind : uint8_t { Paragraph, Heading, ListItem, Quote, CodeBlock };
  Kind kind = Kind::Paragraph;
  /** Heading level 1..6, or list nesting depth (0 = top level). */
  int level = 0;
  /** List marker as displayed ("\xE2\x80\xA2" bullet or "3."); empty otherwise. */
  std::string marker;
  std::vector<Run> runs;

  /** The block's visible text (marker excluded). */
  std::string text() const;
};

struct Document {
  std::vector<Block> blocks;
  bool has_heading = false;
  bool has_strong = false;
  bool has_emphasis = false;
  bool has_code = false;
  bool has_list = false;

  /** Visible text: blocks joined by "\n", list markers included. */
  std::string plain_text() const;
  /** True when one block of plain runs (no styling): renders as a single label. */
  bool is_plain() const;
};

Document parse(std::string_view source);

/** parse(source).plain_text(): the source with Markdown markers stripped. */
std::string strip(std::string_view source);

}  // namespace markdown
}  // namespace a2ui_lvgl
