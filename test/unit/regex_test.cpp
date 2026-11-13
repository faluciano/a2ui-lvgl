// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Tests for the A2UI regex engine. The pattern lists are ported from the
// Python SDK's test_regex_subset.py; `regex_conformance_cases` runs every
// `regex` case of conformance/core/functions.yaml directly against Pattern.

#include <fstream>
#include <sstream>
#include <string>

#include "a2ui/json.h"
#include "a2ui/regex.h"
#include "test_main.h"

using a2ui::Json;
using a2ui::regex::Pattern;

namespace {

std::string repeat(const std::string& s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) out += s;
  return out;
}

/** Empty when the pattern compiles, else the error message. */
std::string compile_error(const std::string& pattern) {
  auto r = Pattern::compile(pattern);
  return r.ok() ? std::string() : r.error().message;
}

bool rejected_with(const std::string& pattern, const char* token) {
  auto r = Pattern::compile(pattern);
  if (r.ok()) {
    std::printf("  pattern /%s/ compiled, expected %s\n", pattern.c_str(), token);
    return false;
  }
  if (r.error().kind != a2ui::ErrorKind::Expression ||
      r.error().message.find(token) == std::string::npos) {
    std::printf("  pattern /%s/: %s (expected %s)\n", pattern.c_str(),
                r.error().message.c_str(), token);
    return false;
  }
  return true;
}

bool accepted(const std::string& pattern) {
  const std::string e = compile_error(pattern);
  if (!e.empty()) std::printf("  pattern /%s/ rejected: %s\n", pattern.c_str(), e.c_str());
  return e.empty();
}

/** Compiles and searches; patterns in these tests are expected to compile. */
bool test(const std::string& pattern, const std::string& value) {
  auto r = Pattern::compile(pattern);
  if (!r.ok()) {
    std::printf("  pattern /%s/ rejected: %s\n", pattern.c_str(), r.error().message.c_str());
    return false;
  }
  return r.value().search(value);
}

}  // namespace

TEST(regex_grammar_acceptance) {
  const char* patterns[] = {
      "",
      "abc",
      R"(^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$)",
      R"(^(?:cat|dog)s?$)",
      R"(^(?=.*[A-Z])(?=.*\d)[A-Za-z\d]{8,}$)",
      R"(^(?!.*\s).+$)",
      R"(^\d{3}-\d{4}$)",
      R"(^[\x20-\x7E\u00A0-\u00FF]+$)",
      R"(^\u{1F600}+$)",
      R"(\bword\b)",
      R"(\Binner\B)",
      R"([\b])",
      R"(\0)",
      R"([\0-\x1F])",
      R"([-a-z])",
      R"([a-z-])",
      R"([^0-9])",
      R"(a*?b+?c??d{1,3}?e{2,}?)",
      R"(\t\n\v\f\r)",
      R"(\^\$\\\.\*\+\?\(\)\[\]\{\}\|\/)",
      R"([\-])",
  };
  for (const char* p : patterns) EXPECT_TRUE(accepted(p));
}

TEST(regex_unsupported_syntax) {
  const char* patterns[] = {
      R"((a)\1)",     R"((a)(b)\2)", R"(\k<id>)",   R"((?<=a)b)",  R"((?<!a)b)",
      R"((?<id>\w+))", R"((?i)abc)", R"((?m)^abc$)", R"(\p{L})",   R"(\P{N})",
      R"(\cA)",       R"(\01)",      R"(\1)",       R"(\a)",       R"(\-)",
      R"([a&&b])",    R"([a--b])",   R"([[a-z]])",  R"([\B])",     R"(a{)",
      R"(a{invalid})", R"(a{1,x})",  R"({)",        R"(})",        R"(])",
      R"(^*)",        R"($+)",       R"(\b?)",      R"((?=a)+)",   R"((?!a)*)",
      R"(a++)",       R"(a*+)",      R"(\uD800)",   R"(\u{DFFF})",
      // Lone surrogates cannot be UTF-8; malformed UTF-8 stands in for them.
      "\xED\xA0\x80", "a\xFF",
  };
  for (const char* p : patterns) EXPECT_TRUE(rejected_with(p, "UNSUPPORTED_SYNTAX"));
}

TEST(regex_syntax_errors) {
  const char* patterns[] = {
      "[invalid(", "(unclosed", "(?:unclosed", "abc)",     "[z-a]",   R"([\d-z])",
      "a{5,2}",    "*abc",      "+abc",        "?abc",     "abc\\",   R"(\xGG)",
      R"(\u123G)", R"(\u{})",   R"(\u{110000})", "a{65536}", "a{0,65536}",
      "[a-",       "[a",        "(?",          "[]",       "[^]",
  };
  for (const char* p : patterns) EXPECT_TRUE(rejected_with(p, "UNSUPPORTED_SYNTAX"));
}

TEST(regex_rule_1_pattern_too_long) {
  EXPECT_TRUE(accepted(repeat("a", 256)));
  EXPECT_TRUE(rejected_with(repeat("a", 257), "PATTERN_TOO_LONG"));
  EXPECT_TRUE(accepted(repeat("\xF0\x9F\x98\x80", 256)));
  EXPECT_TRUE(rejected_with(repeat("\xF0\x9F\x98\x80", 257), "PATTERN_TOO_LONG"));
}

TEST(regex_rule_2_nested_quantifier) {
  for (const char* p : {"(a+)+", "(a*)*", "(a+)*", "(a*){2,5}", "(?:a+){2}", "([a-z]*)*",
                        "(a|b+)+", "((?:a+))+", R"((\d+){2,})"}) {
    EXPECT_TRUE(rejected_with(p, "NESTED_QUANTIFIER"));
  }
  for (const char* p : {"(?:ab+)?c", "(a+){0,1}", R"((\d{1,3}){1,4})", "(a{1,2})+"}) {
    EXPECT_TRUE(accepted(p));
  }
}

TEST(regex_rule_3_overlapping_alternation) {
  for (const char* p : {"(a|ab)+", "(a|ab)*c", R"((\d|[0-5])+)", R"((\w|\d)+)",
                        "([a-m]|[g-z])*"}) {
    EXPECT_TRUE(rejected_with(p, "OVERLAPPING_ALTERNATION"));
  }
  for (const char* p : {"(a|b)*c", "(cat|dog)+", "(a|ab)", "(a|ab)?", R"((\d|[a-f])+)"}) {
    EXPECT_TRUE(accepted(p));
  }
}

TEST(regex_rule_4_adjacent_overlap) {
  for (const char* p : {R"(\d+\d+)", ".*.*", R"(\d+\s*\d+)", R"(\w+\d+)", "a+a+", ".*a*.*",
                        R"(\s*.*)", R"((\d+)(\d+))", R"(\d+\b\d+)"}) {
    EXPECT_TRUE(rejected_with(p, "ADJACENT_OVERLAP"));
  }
  for (const char* p : {R"(\w+\s+\w+)", R"(^[a-z]+@[a-z]+\.com$)", R"(^(?=.*\d).{8,}$)",
                        R"(\d+-\d+)", R"([a-z]+\d+)"}) {
    EXPECT_TRUE(accepted(p));
  }
}

TEST(regex_dialect_semantics) {
  EXPECT_TRUE(!test(R"(^abc$)", "abc\n"));
  EXPECT_TRUE(!test(R"(^\d+$)", "\xD9\xA1\xD9\xA2\xD9\xA3"));  // Arabic-Indic digits.
  EXPECT_TRUE(!test(R"(^\w+$)", "caf\xC3\xA9"));
  EXPECT_TRUE(!test(R"(\b)", "\xC3\xA9"));
  EXPECT_TRUE(test(R"(\B)", "\xC3\xA9"));
  EXPECT_TRUE(test(R"(^.$)", "\xF0\x9F\x98\x80"));
  EXPECT_TRUE(test(R"(^\u{1F600}+$)", "\xF0\x9F\x98\x80\xF0\x9F\x98\x80"));
  EXPECT_TRUE(!test("^a.b$", "a\nb"));
  EXPECT_TRUE(!test("^abc$", "ABC"));
  EXPECT_TRUE(test("abc", "xxabcxx"));
  EXPECT_TRUE(test("", ""));
  EXPECT_TRUE(test("$", "abc"));
  EXPECT_TRUE(!test("^$", "a"));
  EXPECT_TRUE(test(R"(\bword\b)", "a word here"));
  EXPECT_TRUE(!test(R"(\bword\b)", "swordfish"));
  EXPECT_TRUE(test(R"(\Binner\B)", "spinners"));
  EXPECT_TRUE(test("^(?:cat|dog)s?$", "dogs"));
  EXPECT_TRUE(!test("^(?:cat|dog)s?$", "cow"));
  EXPECT_TRUE(test(R"(^\d{3}-\d{4}$)", "555-1234"));
  EXPECT_TRUE(!test(R"(^\d{3}-\d{4}$)", "55-1234"));
  EXPECT_TRUE(test("^a{2,3}$", "aaa"));
  EXPECT_TRUE(!test("^a{2,3}$", "aaaa"));
  EXPECT_TRUE(test("^a{0}b$", "b"));
  EXPECT_TRUE(test("^(?:)*$", ""));
  EXPECT_TRUE(test(R"([\b])", "\b"));
  EXPECT_TRUE(test(R"(^[^0-9]+$)", "abc"));
  EXPECT_TRUE(!test(R"(^[^0-9]+$)", "a1c"));
  EXPECT_TRUE(test(R"(^[\x20-\x7E]+$)", "Hello, world!"));
  EXPECT_TRUE(test(R"(\0)", std::string("a\0b", 3)));
  // Malformed UTF-8 in a value reads as U+FFFD.
  EXPECT_TRUE(test(R"(^.$)", "\xFF"));
  EXPECT_TRUE(test(R"(^\u{FFFD}$)", "\xFF"));
}

TEST(regex_lookaheads) {
  const std::string password = R"(^(?=.*[A-Z])(?=.*\d)[A-Za-z\d]{8,}$)";
  EXPECT_TRUE(test(password, "Secret42abc"));
  EXPECT_TRUE(!test(password, "secret42abc"));
  EXPECT_TRUE(!test(password, "Secretabcd"));
  EXPECT_TRUE(!test(password, "Se42"));
  EXPECT_TRUE(test(R"(^(?!.*\s).+$)", "nospace"));
  EXPECT_TRUE(!test(R"(^(?!.*\s).+$)", "has space"));
  EXPECT_TRUE(test("a(?=b)", "xab"));
  EXPECT_TRUE(!test("a(?=b)", "xac"));
  EXPECT_TRUE(test("a(?!b)", "ab ac"));
  EXPECT_TRUE(test("(?=(?=a)a)a", "a"));
  EXPECT_TRUE(!test("(?=(?!a).)a", "aaa"));
  EXPECT_TRUE(test("(?=(?!a).)b", "ab"));
}

TEST(regex_value_cap) {
  EXPECT_TRUE(test("^a+$", repeat("a", 4096)));
  EXPECT_TRUE(!test("^a+$", repeat("a", 4097)));
  EXPECT_TRUE(test("^\xF0\x9F\x98\x80+$", repeat("\xF0\x9F\x98\x80", 4096)));
  EXPECT_TRUE(!test("^\xF0\x9F\x98\x80+$", repeat("\xF0\x9F\x98\x80", 4097)));
}

TEST(regex_device_limits) {
  // Counted repetitions expand into the program; the expansion is capped.
  EXPECT_TRUE(accepted("^.{0,2000}$"));
  EXPECT_TRUE(rejected_with("a{5000}", "PATTERN_TOO_LONG"));
  EXPECT_TRUE(rejected_with("(?:a{1000}){1000}", "PATTERN_TOO_LONG"));
  EXPECT_TRUE(rejected_with("(?:(?:(?:a{65535}){65535}){65535}){65535}", "PATTERN_TOO_LONG"));
  EXPECT_TRUE(test("^.{0,2000}$", repeat("x", 2000)));
  EXPECT_TRUE(!test("^.{0,2000}$", repeat("x", 2001)));
  // Deep group nesting is fine: the parser keeps open groups on the heap.
  EXPECT_TRUE(test(repeat("(", 120) + "a" + repeat(")", 120), "a"));
  EXPECT_TRUE(accepted(repeat("(?=", 16) + repeat(")", 16)));
  EXPECT_TRUE(rejected_with(repeat("(?=", 17) + repeat(")", 17), "UNSUPPORTED_SYNTAX"));
}

TEST(regex_moved_and_default_patterns) {
  Pattern empty;
  EXPECT_TRUE(!empty.search("a"));
  auto r = Pattern::compile("b");
  EXPECT_TRUE(r.ok());
  Pattern moved = std::move(r).value();
  EXPECT_TRUE(moved.search("abc"));
}

TEST(regex_conformance_cases) {
  const char* path = "build/conformance/core/functions.json";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    unit::report_failure(__FILE__, __LINE__, std::string("cannot read ") + path);
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  auto parsed = Json::parse(ss.str(), 256);
  EXPECT_TRUE(parsed.ok);
  int count = 0;
  for (const Json& tc : parsed.value.as_array()) {
    if (tc["action"].as_string() != "evaluate_function" || tc["function"].as_string() != "regex") {
      continue;
    }
    ++count;
    const std::string name = tc["name"].as_string();
    const Json& args = tc["args"];
    auto r = Pattern::compile(args["pattern"].as_string());
    const Json* expect_error = tc.find("expectError");
    if (!expect_error) expect_error = tc.find("expect_error");
    if (expect_error) {
      bool ok = !r.ok() && r.error().kind == a2ui::ErrorKind::Expression;
      if (ok && (*expect_error)["message"].is_string()) {
        ok = r.error().message.find((*expect_error)["message"].as_string()) != std::string::npos;
      }
      if (!ok) {
        unit::report_failure(__FILE__, __LINE__,
                             name + ": expected error " + expect_error->dump() + ", got " +
                                 (r.ok() ? std::string("success") : r.error().message));
      }
      continue;
    }
    if (!r.ok()) {
      unit::report_failure(__FILE__, __LINE__, name + ": " + r.error().message);
      continue;
    }
    const Json& expect = tc["expect"];
    const bool want = expect.is_object() ? expect["valid"].as_bool() : expect.as_bool();
    if (r.value().search(args["value"].as_string()) != want) {
      unit::report_failure(__FILE__, __LINE__, name + ": expected " + expect.dump());
    }
  }
  std::printf("  regex conformance cases: %d\n", count);
  EXPECT_EQ(count, 34);
}
