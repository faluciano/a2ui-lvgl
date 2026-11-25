// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Conformance runner. Usage:
//   conformance --repo-root third_party/a2ui --suites build/conformance/core
//               [--suite <substr>] [--case <substr>] [--verbose]
//               [--suite-file <path.json>]...   (run these files instead of --suites)
//               [--compact]   (load every catalog with Catalog::from_packed)
//
// Known failures are listed in test/known_failures.txt (one case name per
// line, `#` comments). They are reported but do not fail the run; a known
// failure that passes is reported so the list can be trimmed.

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "harness.h"

namespace harness {
std::map<std::string, ActionFn>& action_registry();
}

using harness::Json;
using harness::Outcome;

namespace {

std::vector<std::string> list_suites(const std::string& dir) {
  std::vector<std::string> files;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string name = e->d_name;
      if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0) {
        files.push_back(dir + "/" + name);
      }
    }
    closedir(d);
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::set<std::string> load_known_failures(const std::string& path) {
  std::set<std::string> names;
  bool ok = false;
  const std::string text = harness::read_file(path, &ok);
  if (!ok) return names;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    while (!line.empty() && isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    while (!line.empty() && isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
    if (!line.empty()) names.insert(line);
    start = end + 1;
  }
  return names;
}

std::string basename_no_ext(const std::string& path) {
  std::string b = path.substr(path.find_last_of('/') + 1);
  return b.substr(0, b.rfind('.'));
}

}  // namespace

int main(int argc, char** argv) {
  harness::Context ctx;
  std::string suites_dir = "build/conformance/core";
  std::string suite_filter, case_filter;
  std::string known_path = "test/known_failures.txt";
  std::vector<std::string> suite_files;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--repo-root") ctx.repo_root = next();
    else if (a == "--suites") suites_dir = next();
    else if (a == "--suite") suite_filter = next();
    else if (a == "--case") case_filter = next();
    else if (a == "--known-failures") known_path = next();
    else if (a == "--suite-file") suite_files.push_back(next());
    else if (a == "--verbose") ctx.verbose = true;
    else if (a == "--compact") harness::set_compact_catalogs(true);
    else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (harness::compact_catalogs()) std::printf("Catalogs: compact (Catalog::from_packed)\n");
  if (ctx.repo_root.empty()) ctx.repo_root = "third_party/a2ui";
  ctx.conformance_root = ctx.repo_root + "/conformance";
  if (auto slash = suites_dir.find_last_of('/'); slash != std::string::npos) {
    ctx.converted_root = suites_dir.substr(0, slash);
  }

  const auto known = load_known_failures(known_path);
  auto& registry = harness::action_registry();

  int total_pass = 0, total_fail = 0, total_unsup = 0, total_xfail = 0;
  std::vector<std::string> failures, xpasses;
  std::map<std::string, int> missing_actions;

  if (suite_files.empty()) suite_files = list_suites(suites_dir);
  for (const auto& file : suite_files) {
    const std::string suite = basename_no_ext(file);
    if (!suite_filter.empty() && suite.find(suite_filter) == std::string::npos) continue;
    std::string err;
    Json cases = harness::load_json_file(file, &err);
    if (!cases.is_array()) {
      std::fprintf(stderr, "%s: %s\n", suite.c_str(), err.empty() ? "not a case list" : err.c_str());
      ++total_fail;
      continue;
    }
    int pass = 0, fail = 0, unsup = 0, xfail = 0;
    for (const auto& c : cases.as_array()) {
      const std::string name = c["name"].as_string();
      if (!case_filter.empty() && name.find(case_filter) == std::string::npos) continue;
      const std::string action = c["action"].as_string();
      Outcome out;
      auto it = registry.find(action);
      if (it == registry.end()) {
        out = Outcome::unsupported("no runner for action '" + action + "'");
        ++missing_actions[action];
      } else {
        out = it->second(c, ctx);
      }
      const bool is_known = known.count(name) > 0;
      switch (out.kind) {
        case Outcome::kPass:
          ++pass;
          if (is_known) xpasses.push_back(suite + "::" + name);
          if (ctx.verbose) {
            std::printf("  PASS %s%s%s\n", name.c_str(), out.detail.empty() ? "" : ": ", out.detail.c_str());
          }
          break;
        case Outcome::kUnsupported:
          ++unsup;
          if (ctx.verbose) std::printf("  SKIP %s: %s\n", name.c_str(), out.detail.c_str());
          break;
        case Outcome::kFail:
          if (is_known) {
            ++xfail;
            if (ctx.verbose) std::printf("  XFAIL %s: %s\n", name.c_str(), out.detail.c_str());
          } else {
            ++fail;
            failures.push_back(suite + "::" + name + "\n      " + out.detail);
          }
          break;
      }
    }
    std::printf("%-34s pass %4d  fail %4d  xfail %4d  unsupported %4d\n", suite.c_str(), pass, fail,
                xfail, unsup);
    total_pass += pass;
    total_fail += fail;
    total_unsup += unsup;
    total_xfail += xfail;
  }

  if (!failures.empty()) {
    std::printf("\nFAILURES\n");
    for (const auto& f : failures) std::printf("  %s\n", f.c_str());
  }
  if (!xpasses.empty()) {
    std::printf("\nKnown failures that now pass (remove from %s):\n", known_path.c_str());
    for (const auto& x : xpasses) std::printf("  %s\n", x.c_str());
  }
  if (!missing_actions.empty() && ctx.verbose) {
    std::printf("\nActions without a runner:\n");
    for (const auto& m : missing_actions) std::printf("  %-28s %d cases\n", m.first.c_str(), m.second);
  }
  std::printf("\nTOTAL pass %d  fail %d  xfail %d  unsupported %d\n", total_pass, total_fail,
              total_xfail, total_unsup);
  return total_fail == 0 ? 0 : 1;
}
