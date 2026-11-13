// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0

#include "test_main.h"

namespace unit {

static int g_failures = 0;
static const char* g_current = "";

std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

void report_failure(const char* file, int line, const std::string& what) {
  ++g_failures;
  std::printf("FAIL %s (%s:%d): %s\n", g_current, file, line, what.c_str());
}

}  // namespace unit

int main() {
  int failed_tests = 0;
  for (const auto& t : unit::registry()) {
    const int before = unit::g_failures;
    unit::g_current = t.name;
    t.fn();
    if (unit::g_failures != before) ++failed_tests;
  }
  std::printf("unit tests: %zu run, %d failed\n", unit::registry().size(), failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
