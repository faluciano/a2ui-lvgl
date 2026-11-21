// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// A tiny self-registering unit test framework (no external dependencies).

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace unit {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();
void report_failure(const char* file, int line, const std::string& what);

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

}  // namespace unit

#define TEST(name)                                              \
  static void name();                                           \
  static unit::Registrar name##_registrar(#name, &name);        \
  static void name()

#define EXPECT_TRUE(cond)                                                  \
  do {                                                                     \
    if (!(cond)) unit::report_failure(__FILE__, __LINE__, "expected " #cond); \
  } while (0)

#define EXPECT_EQ(a, b)                                                              \
  do {                                                                               \
    if (!((a) == (b))) unit::report_failure(__FILE__, __LINE__, "expected " #a " == " #b); \
  } while (0)
