// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Markdown subset parser (see markdown.h). Line-oriented block pass, then a
// recursive inline pass per block. Never fails: anything it does not
// recognise is text.

#include "a2ui_lvgl/markdown.h"

#include <cctype>

namespace a2ui_lvgl {
namespace markdown {

namespace {

constexpr const char* kBullet = "\xE2\x80\xA2";  // U+2022

bool is_blank(std::string_view s) {
  for (char c : s) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

size_t leading_spaces(std::string_view s) {
  size_t n = 0, i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
    n += s[i] == '\t' ? 4 : 1;
    ++i;
  }
  return n;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::string_view ltrim_upto3(std::string_view s) {
  size_t i = 0;
  while (i < 3 && i < s.size() && s[i] == ' ') ++i;
  return s.substr(i);
}

bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool is_ascii_punct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }

// --- Inline -------------------------------------------------------------------

class InlineParser {
 public:
  InlineParser(Document* doc, std::vector<Run>* out) : doc_(doc), out_(out) {}

  void parse(std::string_view s, uint8_t style, int depth = 0) {
    std::string buf;
    auto flush = [&] {
      if (buf.empty()) return;
      emit(buf, style);
      buf.clear();
    };
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
      const char c = s[i];
      if (c == '\\' && i + 1 < n && is_ascii_punct(s[i + 1])) {
        buf += s[i + 1];
        i += 2;
        continue;
      }
      if (c == '`') {
        size_t k = 0;
        while (i + k < n && s[i + k] == '`') ++k;
        size_t close = find_backticks(s, i + k, k);
        if (close != std::string_view::npos) {
          flush();
          std::string_view code = s.substr(i + k, close - (i + k));
          if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && !is_blank(code)) {
            code = code.substr(1, code.size() - 2);
          }
          emit(std::string(code), static_cast<uint8_t>(style | kCode));
          doc_->has_code = true;
          i = close + k;
        } else {
          buf.append(s.substr(i, k));
          i += k;
        }
        continue;
      }
      if (depth < 8 && (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '['))) {
        const size_t open = c == '!' ? i + 1 : i;
        std::string_view label;
        size_t end = 0;
        if (parse_link(s, open, &label, &end)) {
          flush();
          parse(label, style, depth + 1);
          i = end;
          continue;
        }
      }
      if (c == '<') {
        size_t close = s.find('>', i + 1);
        if (close != std::string_view::npos) {
          std::string_view inner = s.substr(i + 1, close - i - 1);
          if (is_autolink(inner)) {
            buf.append(inner);
            i = close + 1;
            continue;
          }
        }
      }
      if ((c == '*' || c == '_') && depth < 8) {
        size_t k = 0;
        while (i + k < n && s[i + k] == c) ++k;
        const bool prev_alnum = i > 0 && is_alnum(s[i - 1]);
        const bool next_ok = i + k < n && s[i + k] != ' ' && s[i + k] != '\t';
        if (next_ok && !(c == '_' && prev_alnum)) {
          size_t content_end = 0, after = 0;
          uint8_t add = 0;
          if (k >= 3 && find_close(s, i + 3, c, 3, &content_end)) {
            add = kStrong | kEmphasis;
            after = content_end + 3;
          } else if (k >= 2 && find_close(s, i + 2, c, 2, &content_end)) {
            add = kStrong;
            after = content_end + 2;
          } else if (find_close(s, i + 1, c, 1, &content_end)) {
            add = kEmphasis;
            after = content_end + 1;
          }
          if (add != 0) {
            const size_t width = add == (kStrong | kEmphasis) ? 3 : (add == kStrong ? 2 : 1);
            // Extra opening markers beyond what matched stay literal.
            flush();
            if (k > width && add != (kStrong | kEmphasis)) {
              // e.g. "***x**" -> "*" + strong("x"); keep it simple: literal prefix.
              buf.append(k - width, c);
              flush();
            }
            const size_t start = i + (k > width && add != (kStrong | kEmphasis) ? k : width);
            if (add & kStrong) doc_->has_strong = true;
            if (add & kEmphasis) doc_->has_emphasis = true;
            parse(s.substr(start, content_end - start), static_cast<uint8_t>(style | add), depth + 1);
            i = after;
            continue;
          }
        }
        buf.append(k, c);
        i += k;
        continue;
      }
      buf += c;
      ++i;
    }
    flush();
  }

 private:
  void emit(const std::string& text, uint8_t style) {
    if (text.empty()) return;
    if (!out_->empty() && out_->back().style == style) {
      out_->back().text += text;
    } else {
      out_->push_back(Run{text, style});
    }
  }

  static size_t find_backticks(std::string_view s, size_t from, size_t k) {
    size_t i = from;
    while (i < s.size()) {
      if (s[i] != '`') {
        ++i;
        continue;
      }
      size_t m = 0;
      while (i + m < s.size() && s[i + m] == '`') ++m;
      if (m == k) return i;
      i += m;
    }
    return std::string_view::npos;
  }

  /** Finds a closing run of exactly `width` `c` markers after `from`; content must be non-empty and not end in space. */
  static bool find_close(std::string_view s, size_t from, char c, size_t width, size_t* content_end) {
    size_t i = from;
    while (i < s.size()) {
      if (s[i] == '\\') {
        i += 2;
        continue;
      }
      if (s[i] == '`') {  // Skip code spans: markers inside them do not close.
        size_t k = 0;
        while (i + k < s.size() && s[i + k] == '`') ++k;
        size_t close = find_backticks(s, i + k, k);
        i = close == std::string_view::npos ? i + k : close + k;
        continue;
      }
      if (s[i] != c) {
        ++i;
        continue;
      }
      size_t m = 0;
      while (i + m < s.size() && s[i + m] == c) ++m;
      const bool prev_space = i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t';
      const bool next_alnum = i + m < s.size() && is_alnum(s[i + m]);
      if (i > from && !prev_space && !(c == '_' && next_alnum)) {
        if (m == width || (width == 1 && m == 3) || (width == 2 && m == 3)) {
          *content_end = i;
          return true;
        }
      }
      i += m;  // A run of another width (nested emphasis) is skipped whole.
    }
    return false;
  }

  /** `[label](dest)` starting at `open` ('['); sets the label and the index after ')'. */
  static bool parse_link(std::string_view s, size_t open, std::string_view* label, size_t* end) {
    int depth = 0;
    size_t i = open;
    size_t close = std::string_view::npos;
    for (; i < s.size(); ++i) {
      if (s[i] == '\\') {
        ++i;
        continue;
      }
      if (s[i] == '[') ++depth;
      if (s[i] == ']' && --depth == 0) {
        close = i;
        break;
      }
    }
    if (close == std::string_view::npos || close + 1 >= s.size() || s[close + 1] != '(') return false;
    int parens = 0;
    for (size_t j = close + 1; j < s.size(); ++j) {
      if (s[j] == '\\') {
        ++j;
        continue;
      }
      if (s[j] == '(') ++parens;
      if (s[j] == ')' && --parens == 0) {
        *label = s.substr(open + 1, close - open - 1);
        *end = j + 1;
        return true;
      }
    }
    return false;
  }

  static bool is_autolink(std::string_view inner) {
    if (inner.empty() || inner.find(' ') != std::string_view::npos) return false;
    size_t colon = inner.find(':');
    if (colon == std::string_view::npos || colon < 2) return false;
    for (size_t i = 0; i < colon; ++i) {
      char ch = inner[i];
      if (!is_alnum(ch) && ch != '+' && ch != '-' && ch != '.') return false;
    }
    return std::isalpha(static_cast<unsigned char>(inner[0])) != 0;
  }

  Document* doc_;
  std::vector<Run>* out_;
};

// --- Blocks -------------------------------------------------------------------

bool thematic_break(std::string_view line) {
  std::string_view s = ltrim_upto3(line);
  if (s.empty()) return false;
  const char c = s[0];
  if (c != '-' && c != '*' && c != '_') return false;
  int count = 0;
  for (char ch : s) {
    if (ch == c) {
      ++count;
    } else if (ch != ' ' && ch != '\t') {
      return false;
    }
  }
  return count >= 3;
}

int heading_level(std::string_view line, std::string_view* rest) {
  std::string_view s = ltrim_upto3(line);
  int level = 0;
  while (level < static_cast<int>(s.size()) && s[level] == '#') ++level;
  if (level == 0 || level > 6) return 0;
  if (static_cast<size_t>(level) < s.size() && s[level] != ' ' && s[level] != '\t') return 0;
  std::string_view r = trim(s.substr(level));
  // Optional closing sequence of #s preceded by a space.
  size_t e = r.size();
  while (e > 0 && r[e - 1] == '#') --e;
  if (e < r.size() && (e == 0 || r[e - 1] == ' ' || r[e - 1] == '\t')) r = trim(r.substr(0, e));
  *rest = r;
  return level;
}

bool list_item(std::string_view line, int* indent, std::string* marker, std::string_view* rest) {
  const size_t lead = leading_spaces(line);
  std::string_view s = trim(line);
  if (s.empty()) return false;
  if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && s.size() >= 2 && (s[1] == ' ' || s[1] == '\t')) {
    *marker = kBullet;
    *rest = trim(s.substr(2));
    *indent = static_cast<int>(lead);
    return true;
  }
  size_t d = 0;
  while (d < s.size() && d < 9 && std::isdigit(static_cast<unsigned char>(s[d]))) ++d;
  if (d > 0 && d + 1 < s.size() && (s[d] == '.' || s[d] == ')') && (s[d + 1] == ' ' || s[d + 1] == '\t')) {
    *marker = std::string(s.substr(0, d)) + ".";
    *rest = trim(s.substr(d + 2));
    *indent = static_cast<int>(lead);
    return true;
  }
  return false;
}

bool fence(std::string_view line, char* fence_char, size_t* fence_len) {
  std::string_view s = ltrim_upto3(line);
  if (s.size() < 3 || (s[0] != '`' && s[0] != '~')) return false;
  size_t k = 0;
  while (k < s.size() && s[k] == s[0]) ++k;
  if (k < 3) return false;
  *fence_char = s[0];
  *fence_len = k;
  return true;
}

class BlockParser {
 public:
  explicit BlockParser(Document* doc) : doc_(doc) {}

  void run(std::string_view src) {
    size_t pos = 0;
    while (pos <= src.size()) {
      size_t nl = src.find('\n', pos);
      if (nl == std::string_view::npos) nl = src.size();
      std::string_view line = src.substr(pos, nl - pos);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      feed(line);
      if (nl == src.size()) break;
      pos = nl + 1;
    }
    if (in_code_) {
      close_code();
    }
    close_open();
  }

 private:
  void feed(std::string_view line) {
    if (in_code_) {
      char fc = 0;
      size_t fl = 0;
      if (fence(line, &fc, &fl) && fc == fence_char_ && fl >= fence_len_ && is_blank(ltrim_upto3(line).substr(fl))) {
        close_code();
        return;
      }
      if (!code_.empty()) code_ += '\n';
      code_.append(line);
      return;
    }
    if (is_blank(line)) {
      close_open();
      return;
    }
    char fc = 0;
    size_t fl = 0;
    if (fence(line, &fc, &fl)) {
      close_open();
      in_code_ = true;
      fence_char_ = fc;
      fence_len_ = fl;
      code_.clear();
      return;
    }
    // Setext underline for a pending paragraph.
    if (open_ == Block::Kind::Paragraph && has_open_) {
      std::string_view t = trim(line);
      if (!t.empty() && (t.find_first_not_of('=') == std::string_view::npos ||
                         t.find_first_not_of('-') == std::string_view::npos)) {
        open_kind_override_heading_ = t[0] == '=' ? 1 : 2;
        close_open();
        return;
      }
    }
    if (thematic_break(line)) {
      close_open();
      return;
    }
    std::string_view rest;
    if (int level = heading_level(line, &rest)) {
      close_open();
      Block b;
      b.kind = Block::Kind::Heading;
      b.level = level;
      add_block(std::move(b), rest);
      doc_->has_heading = true;
      return;
    }
    int indent = 0;
    std::string marker;
    if (list_item(line, &indent, &marker, &rest)) {
      close_open();
      open_ = Block::Kind::ListItem;
      has_open_ = true;
      open_level_ = indent >= 2 ? (indent / 2 > 4 ? 4 : indent / 2) : 0;
      open_marker_ = marker;
      text_.assign(rest);
      hard_break_pending_ = ends_with_hard_break(line, &text_);
      doc_->has_list = true;
      return;
    }
    std::string_view s = ltrim_upto3(line);
    if (!s.empty() && s[0] == '>') {
      std::string_view q = s.substr(1);
      if (!q.empty() && q[0] == ' ') q.remove_prefix(1);
      if (!(has_open_ && open_ == Block::Kind::Quote)) {
        close_open();
        open_ = Block::Kind::Quote;
        has_open_ = true;
        text_.clear();
      }
      append_line(q, line);
      return;
    }
    // Paragraph text, or a continuation of the open block.
    if (!has_open_) {
      open_ = Block::Kind::Paragraph;
      has_open_ = true;
      text_.clear();
    }
    append_line(trim(line), line);
  }

  static bool ends_with_hard_break(std::string_view raw, std::string* text) {
    if (!raw.empty() && raw.back() == '\\') {
      if (!text->empty() && text->back() == '\\') text->pop_back();
      return true;
    }
    return raw.size() >= 2 && raw[raw.size() - 1] == ' ' && raw[raw.size() - 2] == ' ';
  }

  void append_line(std::string_view content, std::string_view raw) {
    content = trim(content);
    if (!text_.empty()) text_ += hard_break_pending_ ? '\n' : ' ';
    text_.append(content);
    hard_break_pending_ = ends_with_hard_break(raw, &text_);
  }

  void close_open() {
    if (!has_open_) return;
    Block b;
    b.kind = open_;
    if (open_ == Block::Kind::Paragraph && open_kind_override_heading_ > 0) {
      b.kind = Block::Kind::Heading;
      b.level = open_kind_override_heading_;
      doc_->has_heading = true;
    }
    if (open_ == Block::Kind::ListItem) {
      b.level = open_level_;
      b.marker = open_marker_;
    }
    open_kind_override_heading_ = 0;
    has_open_ = false;
    hard_break_pending_ = false;
    std::string text = std::move(text_);
    text_.clear();
    add_block(std::move(b), text);
  }

  void close_code() {
    in_code_ = false;
    Block b;
    b.kind = Block::Kind::CodeBlock;
    if (!code_.empty()) b.runs.push_back(Run{code_, kCode});
    doc_->has_code = true;
    doc_->blocks.push_back(std::move(b));
    code_.clear();
  }

  void add_block(Block b, std::string_view text) {
    InlineParser(doc_, &b.runs).parse(text, kPlain);
    doc_->blocks.push_back(std::move(b));
  }

  Document* doc_;
  bool has_open_ = false;
  Block::Kind open_ = Block::Kind::Paragraph;
  int open_level_ = 0;
  int open_kind_override_heading_ = 0;
  std::string open_marker_;
  std::string text_;
  bool hard_break_pending_ = false;
  bool in_code_ = false;
  char fence_char_ = '`';
  size_t fence_len_ = 3;
  std::string code_;
};

}  // namespace

std::string Block::text() const {
  std::string out;
  for (const auto& r : runs) out += r.text;
  return out;
}

std::string Document::plain_text() const {
  std::string out;
  for (size_t i = 0; i < blocks.size(); ++i) {
    if (i > 0) out += '\n';
    const Block& b = blocks[i];
    if (b.kind == Block::Kind::ListItem) {
      out.append(static_cast<size_t>(b.level) * 2, ' ');
      out += b.marker;
      out += ' ';
    }
    out += b.text();
  }
  return out;
}

bool Document::is_plain() const {
  if (blocks.size() > 1) return false;
  if (blocks.empty()) return true;
  const Block& b = blocks[0];
  if (b.kind != Block::Kind::Paragraph) return false;
  for (const auto& r : b.runs) {
    if (r.style != kPlain) return false;
  }
  return true;
}

Document parse(std::string_view source) {
  Document doc;
  BlockParser(&doc).run(source);
  return doc;
}

std::string strip(std::string_view source) { return parse(source).plain_text(); }

}  // namespace markdown
}  // namespace a2ui_lvgl
