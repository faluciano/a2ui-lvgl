// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Port of the reference `${...}` template parsers (Swift ExpressionParser,
// TypeScript expression_parser.ts, Python expression_parser.py). Where they
// differ, this follows Swift and TypeScript: an unclosed string literal is an
// error, and whitespace is skipped before the trailing-characters check.
//
// The scanner works on code points of UTF-8 input. Lengths are counted in
// code points, as Python does.
//
// Stack use: each nesting level (a nested `${` or a function-call argument)
// costs a few small frames; `max_depth` (default 100, shared by every SDK)
// bounds it. Devices with small task stacks can pass a lower limit.

#include <cmath>
#include <cstdlib>
#include <string>

#include "a2ui/expressions.h"
#include "a2ui/identifier.h"

namespace a2ui {
namespace expressions {
namespace {

constexpr size_t kDefaultMaxTemplateLength = 10000;
constexpr size_t kDefaultMaxParts = 1000;
constexpr int kDefaultMaxDepth = 100;
constexpr uint32_t kEnd = 0xFFFFFFFFu;  // peek() past the end of input.

Error parse_error(std::string message) {
  return make_error(ErrorKind::Expression, std::move(message));
}

Error depth_error() { return parse_error("Max recursion depth reached in parse"); }

/** ECMAScript WhiteSpace and LineTerminator (what `\s` and trim() use). */
bool is_space(uint32_t c) {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
         c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

bool is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

bool is_ascii_letter(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool is_identifier_char(uint32_t c) { return c != kEnd && is_xid_continue(c); }

bool is_path_char(uint32_t c) { return is_identifier_char(c) || c == '/' || c == '.' || c == '-'; }

class Scanner {
 public:
  explicit Scanner(std::string_view input) : input_(input) {}

  bool at_end() const { return pos_ >= input_.size(); }
  size_t pos() const { return pos_; }
  std::string_view input() const { return input_; }
  std::string_view slice(size_t start) const { return input_.substr(start, pos_ - start); }
  std::string_view rest() const { return input_.substr(pos_); }

  /** The code point `offset` code points ahead, or kEnd. */
  uint32_t peek(size_t offset = 0) const {
    size_t p = pos_;
    for (;;) {
      if (p >= input_.size()) return kEnd;
      const uint32_t c = decode_utf8(input_, p);
      if (offset-- == 0) return c;
    }
  }

  /** Advances one code point and returns it (kEnd at the end). */
  uint32_t next() {
    if (at_end()) return kEnd;
    return decode_utf8(input_, pos_);
  }

  void advance(size_t count = 1) {
    while (count-- > 0 && !at_end()) decode_utf8(input_, pos_);
  }

  /** ASCII-only prefix test at the current position. */
  bool matches(std::string_view s) const { return input_.substr(pos_, s.size()) == s; }

  bool match(uint32_t c) {
    if (peek() != c) return false;
    advance();
    return true;
  }

  bool matches_keyword(std::string_view keyword) {
    if (!matches(keyword)) return false;
    size_t p = pos_ + keyword.size();
    if (p < input_.size() && is_identifier_char(decode_utf8(input_, p))) return false;
    pos_ += keyword.size();
    return true;
  }

  void skip_whitespace() {
    while (!at_end() && is_space(peek())) advance();
  }

 private:
  std::string_view input_;
  size_t pos_ = 0;
};

std::string_view trim(std::string_view s) {
  size_t start = 0;
  while (start < s.size()) {
    size_t p = start;
    if (!is_space(decode_utf8(s, p))) break;
    start = p;
  }
  size_t end = start;  // End of the last non-space code point.
  for (size_t p = start; p < s.size();) {
    if (!is_space(decode_utf8(s, p))) end = p;
  }
  return s.substr(start, end - start);
}

/** ^[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$ */
bool is_valid_number_literal(std::string_view s) {
  size_t i = 0;
  const size_t n = s.size();
  auto digits = [&]() {
    const size_t start = i;
    while (i < n && is_digit(static_cast<unsigned char>(s[i]))) ++i;
    return i - start;
  };
  if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
  if (digits() > 0) {
    if (i < n && s[i] == '.') {
      ++i;
      digits();
    }
  } else {
    if (i >= n || s[i] != '.') return false;
    ++i;
    if (digits() == 0) return false;
  }
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
    if (digits() == 0) return false;
  }
  return i == n;
}

class Parser {
 public:
  explicit Parser(const ParseLimits& limits)
      : max_length_(limits.max_template_length ? limits.max_template_length
                                               : kDefaultMaxTemplateLength),
        max_parts_(limits.max_parts ? limits.max_parts : kDefaultMaxParts),
        max_depth_(limits.max_depth > 0 ? limits.max_depth : kDefaultMaxDepth) {}

  Result<Json> parse(std::string_view input, int depth) {
    if (depth > max_depth_) return depth_error();
    const size_t length = utf8_length(input);
    if (length > max_length_) {
      return parse_error("Expression template length (" + std::to_string(length) +
                         ") exceeds maximum limit (" + std::to_string(max_length_) + ")");
    }
    Json parts = Json::array();
    if (input.empty()) return parts;
    if (input.find("${") == std::string_view::npos) {
      parts.push_back(Json(input));
      return parts;
    }

    Scanner scanner(input);
    while (!scanner.at_end()) {
      if (parts.size() >= max_parts_) {
        return parse_error("Expression parts count exceeds maximum limit (" +
                           std::to_string(max_parts_) + ")");
      }
      if (scanner.matches("${")) {
        scanner.advance(2);
        auto content = extract_interpolation_content(scanner);
        if (!content.ok()) return content.take_error();
        auto parsed = parse_expression(content.value(), depth + 1);
        if (!parsed.ok()) return parsed.take_error();
        Json& v = parsed.value();
        const bool empty = v.is_null() || (v.is_string() && v.as_string().empty());
        if (!empty) parts.push_back(std::move(v));
      } else if (scanner.matches("\\${")) {
        scanner.advance(3);
        parts.push_back(Json("${"));
      } else {
        const size_t start = scanner.pos();
        while (!scanner.at_end() && !scanner.matches("${") && !scanner.matches("\\${")) {
          scanner.advance();
        }
        if (scanner.pos() > start) parts.push_back(Json(scanner.slice(start)));
      }
    }
    return parts;
  }

 private:
  Result<std::string_view> extract_interpolation_content(Scanner& scanner) {
    const size_t start = scanner.pos();
    int balance = 1;
    size_t end = start;
    while (!scanner.at_end() && balance > 0) {
      end = scanner.pos();
      const uint32_t c = scanner.next();
      if (c == '{') {
        ++balance;
      } else if (c == '}') {
        --balance;
      } else if (c == '\'' || c == '"') {
        while (!scanner.at_end()) {
          const uint32_t q = scanner.next();
          if (q == '\\') {
            scanner.advance();
          } else if (q == c) {
            break;
          }
        }
      }
    }
    if (balance > 0) return parse_error("Unclosed interpolation: missing '}'");
    return scanner.input().substr(start, end - start);
  }

  Result<Json> parse_expression(std::string_view expr, int depth) {
    if (depth > max_depth_) return depth_error();
    const std::string_view trimmed = trim(expr);
    if (trimmed.empty()) return Json("");
    Scanner scanner(trimmed);
    auto result = parse_expression_internal(scanner, depth);
    if (!result.ok()) return result;
    scanner.skip_whitespace();
    if (!scanner.at_end()) {
      return parse_error("Unexpected characters at end of expression: '" +
                         std::string(scanner.rest()) + "'");
    }
    return result;
  }

  Result<Json> parse_expression_internal(Scanner& scanner, int depth) {
    // Both recursive paths pass through here: interpolations nested inside an
    // interpolation and function-call arguments. Checking here counts both.
    if (depth > max_depth_) return depth_error();
    scanner.skip_whitespace();
    if (scanner.at_end()) return Json("");

    if (scanner.matches("${")) {
      scanner.advance(2);
      auto content = extract_interpolation_content(scanner);
      if (!content.ok()) return content.take_error();
      return parse_expression(content.value(), depth + 1);
    }

    const uint32_t c = scanner.peek();
    if (c == '\'' || c == '"') return parse_string_literal(scanner);
    if (is_number_start(scanner)) return parse_number_literal(scanner);
    if (scanner.matches_keyword("true")) return Json(true);
    if (scanner.matches_keyword("false")) return Json(false);
    if (scanner.matches_keyword("null")) return Json("");

    auto token = scan_path_or_identifier(scanner);
    if (!token.ok()) return token.take_error();
    scanner.skip_whitespace();
    if (scanner.peek() == '(') return parse_function_call(token.value(), scanner, depth);
    if (!token.value().empty() && token.value()[0] == '@') {
      return parse_error("Expected '(' after function name '" + std::string(token.value()) +
                         "'");
    }
    if (token.value().empty()) return Json("");
    return Json::object({{"path", Json(token.value())}});
  }

  Result<Json> parse_function_call(std::string_view name, Scanner& scanner, int depth) {
    scanner.match('(');
    scanner.skip_whitespace();
    Json args = Json::object();
    while (!scanner.at_end() && scanner.peek() != ')') {
      const std::string_view arg_name = scan_identifier(scanner);
      scanner.skip_whitespace();
      if (!scanner.match(':')) {
        return parse_error("Expected ':' after argument name '" + std::string(arg_name) +
                           "' in function '" + std::string(name) + "'");
      }
      scanner.skip_whitespace();
      auto value = parse_expression_internal(scanner, depth + 1);
      if (!value.ok()) return value;
      args.set(arg_name, std::move(value).value());
      scanner.skip_whitespace();
      if (scanner.peek() == ',') {
        scanner.advance();
        scanner.skip_whitespace();
      }
    }
    if (!scanner.match(')')) {
      return parse_error("Expected ')' after function arguments for '" + std::string(name) + "'");
    }
    Json call = Json::object();
    call.set("call", Json(name));
    call.set("args", std::move(args));
    call.set("returnType", Json("any"));
    return call;
  }

  static Result<std::string_view> scan_path_or_identifier(Scanner& scanner) {
    const size_t start = scanner.pos();
    // A leading `@` starts a system function name such as `@index`, which
    // every engine restricts to ASCII [A-Za-z_][A-Za-z0-9_]*.
    if (scanner.peek() == '@') {
      const uint32_t first = scanner.peek(1);
      if (first != '_' && !is_ascii_letter(first)) {
        return parse_error("Invalid identifier starting with '@' in expression");
      }
      scanner.advance();
      for (uint32_t c = scanner.peek(); c == '_' || is_ascii_letter(c) || is_digit(c);
           c = scanner.peek()) {
        scanner.advance();
      }
      return scanner.slice(start);
    }
    while (is_path_char(scanner.peek())) scanner.advance();
    return scanner.slice(start);
  }

  static std::string_view scan_identifier(Scanner& scanner) {
    const size_t start = scanner.pos();
    while (is_identifier_char(scanner.peek())) scanner.advance();
    return scanner.slice(start);
  }

  static Result<Json> parse_string_literal(Scanner& scanner) {
    const uint32_t quote = scanner.next();
    std::string result;
    while (!scanner.at_end()) {
      const size_t start = scanner.pos();
      const uint32_t c = scanner.next();
      if (c == '\\') {
        if (scanner.at_end()) break;
        const size_t esc = scanner.pos();
        switch (scanner.next()) {
          case 'n': result += '\n'; break;
          case 't': result += '\t'; break;
          case 'r': result += '\r'; break;
          default: result += scanner.slice(esc); break;
        }
      } else if (c == quote) {
        return Json(std::move(result));
      } else {
        result += scanner.slice(start);
      }
    }
    return parse_error(std::string("Unclosed string literal: missing closing ") +
                       static_cast<char>(quote));
  }

  /**
   * A number starts at a digit, at a `.` followed by a digit, or at a sign
   * followed by either. The grammar has no arithmetic operators, so a sign
   * here can only belong to a literal; a `-` or `.` inside a path such as
   * `a-1` never reaches this check because the path scanner consumes it.
   */
  static bool is_number_start(const Scanner& scanner) {
    const uint32_t first = scanner.peek();
    const size_t offset = (first == '-' || first == '+') ? 1 : 0;
    if (is_digit(scanner.peek(offset))) return true;
    return scanner.peek(offset) == '.' && is_digit(scanner.peek(offset + 1));
  }

  static Result<Json> parse_number_literal(Scanner& scanner) {
    const size_t start = scanner.pos();
    if (scanner.peek() == '-' || scanner.peek() == '+') scanner.advance();
    while (is_digit(scanner.peek()) || scanner.peek() == '.') scanner.advance();
    // Consume a malformed exponent such as `1e` or `1e+` too, so it is
    // reported as an invalid literal rather than as trailing characters.
    if (scanner.peek() == 'e' || scanner.peek() == 'E') {
      scanner.advance();
      if (scanner.peek() == '+' || scanner.peek() == '-') scanner.advance();
      while (is_digit(scanner.peek())) scanner.advance();
    }
    const std::string text(scanner.slice(start));
    if (!is_valid_number_literal(text)) {
      return parse_error("Invalid number literal: '" + text + "'");
    }
    // The text is plain decimal, so strtod is exact for integers up to 2^53
    // and needs no locale beyond the default "C" one.
    const double value = std::strtod(text.c_str(), nullptr);
    if (!std::isfinite(value)) {
      return parse_error("Number literal is out of range: '" + text + "'");
    }
    return Json(value);
  }

  size_t max_length_;
  size_t max_parts_;
  int max_depth_;
};

}  // namespace

Result<Json> parse_template(std::string_view text, const ParseLimits& limits) {
  return Parser(limits).parse(text, 0);
}

}  // namespace expressions
}  // namespace a2ui
