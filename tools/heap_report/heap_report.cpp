// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Host heap report for a2ui_core: how much the basic catalog and a typical
// card allocate, step by step. Counts every operator new that is still live
// after each step (the libraries allocate only through new).
//
//   make heap-report      (runs all three modes)
//   heap_report [--load | --compact] [card.json]
//
// Modes: the default loads the catalog as 0.1.0 documented
// (Catalog::from_json on the cached spec::parsed document); --load uses
// basic_catalog::load(false) (a parsed copy moved into the catalog);
// --compact uses basic_catalog::load(true) (Catalog::from_packed).
//
// Host figures are for a 64-bit build. The "est. 32-bit" column models the
// same allocations on a 32-bit ESP32 (std::string 24 bytes, Json 16, a heap
// block header of 4 bytes rounded up to 4) for the parsed Json trees; the
// figures in docs/porting.md come from QEMU (tools/stack_probe).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/json.h"
#include "a2ui/message_processor.h"
#include "a2ui/schema_node.h"
#include "a2ui/spec_documents.h"

// --- counting allocator ------------------------------------------------------

namespace {

struct Counters {
  long long live_bytes = 0;
  long long live_blocks = 0;
  long long peak_bytes = 0;  // Highest live_bytes since the last step.
};
Counters g_count;

constexpr size_t kHeader = 16;  // Keeps the payload 16-byte aligned.

}  // namespace

void* operator new(std::size_t size) {
  void* raw = std::malloc(size + kHeader);
  if (raw == nullptr) std::abort();
  *static_cast<std::size_t*>(raw) = size;
  g_count.live_bytes += static_cast<long long>(size);
  g_count.live_blocks += 1;
  if (g_count.live_bytes > g_count.peak_bytes) g_count.peak_bytes = g_count.live_bytes;
  return static_cast<char*>(raw) + kHeader;
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept {
  if (p == nullptr) return;
  void* raw = static_cast<char*>(p) - kHeader;
  g_count.live_bytes -= static_cast<long long>(*static_cast<std::size_t*>(raw));
  g_count.live_blocks -= 1;
  std::free(raw);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

// --- Json tree model -----------------------------------------------------------

namespace {

struct TreeStats {
  long nodes = 0;
  long strings = 0;          // String values and object keys.
  long string_bytes = 0;     // Their characters.
  long long_strings = 0;     // Longer than the 15-byte small-string buffer.
  long containers = 0;
  long members = 0;
  long elements = 0;
  long annotation_bytes = 0; // description/title/examples/$comment subtrees (text only).
  // Modelled 32-bit heap: payload bytes and block count.
  long long est32_bytes = 0;
  long est32_blocks = 0;
};

constexpr long kJson32 = 16;
constexpr long kString32 = 24;
constexpr long kVector32 = 12;
constexpr long kMember32 = kString32 + kJson32;

long round_block(long n) { return ((n + 3) / 4) * 4 + 4; }

void count_string(const std::string& s, TreeStats* st, bool owned_by_json) {
  st->strings++;
  st->string_bytes += static_cast<long>(s.size());
  if (owned_by_json) {  // Json holds a std::string* (one block).
    st->est32_bytes += round_block(kString32);
    st->est32_blocks++;
  }
  if (s.size() > 15) {
    st->long_strings++;
    st->est32_bytes += round_block(static_cast<long>(s.size()) + 1);
    st->est32_blocks++;
  }
}

long text_bytes(const a2ui::Json& j) {
  if (j.is_string()) return static_cast<long>(j.as_string().size());
  long n = 0;
  if (j.is_array()) for (const auto& e : j.as_array()) n += text_bytes(e);
  if (j.is_object()) for (const auto& m : j.as_object()) n += static_cast<long>(m.first.size()) + text_bytes(m.second);
  return n;
}

void walk(const a2ui::Json& j, TreeStats* st) {
  st->nodes++;
  switch (j.type()) {
    case a2ui::Json::Type::String:
      count_string(j.as_string(), st, true);
      break;
    case a2ui::Json::Type::Array:
      st->containers++;
      st->elements += static_cast<long>(j.size());
      st->est32_bytes += round_block(kVector32);
      st->est32_blocks++;
      if (j.size() > 0) {
        st->est32_bytes += round_block(kJson32 * static_cast<long>(j.as_array().capacity()));
        st->est32_blocks++;
      }
      for (const auto& e : j.as_array()) walk(e, st);
      break;
    case a2ui::Json::Type::Object:
      st->containers++;
      st->members += static_cast<long>(j.size());
      st->est32_bytes += round_block(kVector32);
      st->est32_blocks++;
      if (j.size() > 0) {
        st->est32_bytes += round_block(kMember32 * static_cast<long>(j.as_object().capacity()));
        st->est32_blocks++;
      }
      for (const auto& m : j.as_object()) {
        count_string(m.first, st, false);
        if (m.first == "description" || m.first == "title" || m.first == "examples" || m.first == "$comment") {
          st->annotation_bytes += text_bytes(m.second);
        }
        walk(m.second, st);
      }
      break;
    default:
      break;
  }
}

void print_tree(const char* name, std::string_view text, const a2ui::Json& j) {
  TreeStats st;
  walk(j, &st);
  std::printf("  %-22s text %6zu B | nodes %5ld, containers %4ld, strings %5ld (%ld chars, %ld long) | "
              "annotations %5ld chars | est. 32-bit %6lld B in %ld blocks\n",
              name, text.size(), st.nodes, st.containers, st.strings, st.string_bytes, st.long_strings,
              st.annotation_bytes, st.est32_bytes, st.est32_blocks);
}

// --- steps ------------------------------------------------------------------------

Counters g_mark;

void step(const char* what) {
  const long long db = g_count.live_bytes - g_mark.live_bytes;
  const long long dn = g_count.live_blocks - g_mark.live_blocks;
  std::printf("  %-44s %+9lld B %+7lld blocks   (live %8lld B, %6lld blocks; peak %+8lld B)\n", what, db, dn,
              g_count.live_bytes, g_count.live_blocks, g_count.peak_bytes - g_mark.live_bytes);
  g_count.peak_bytes = g_count.live_bytes;
  g_mark = g_count;
}

std::string read_file(const char* path) {
  std::string out;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  bool compact = false;
  bool load = false;
  const char* card_path = "examples/basic_surface/main/coffee_order.json";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--compact") == 0) {
      compact = true;
    } else if (std::strcmp(argv[i], "--load") == 0) {
      load = true;
    } else {
      card_path = argv[i];
    }
  }
  const std::string card = read_file(card_path);
  if (card.empty()) {
    std::fprintf(stderr, "cannot read %s\n", card_path);
    return 1;
  }

  using a2ui::spec::Document;
  if (!compact && !load) {
    std::printf("Spec documents as Json trees:\n");
    for (auto d : {Document::BasicCatalogV1, Document::CommonTypesV1_0, Document::AgentToRendererV1_0}) {
      const char* name = d == Document::BasicCatalogV1    ? "basic catalog"
                         : d == Document::CommonTypesV1_0 ? "common_types"
                                                          : "agent_to_renderer";
      auto parsed = a2ui::Json::parse(a2ui::spec::text(d));
      print_tree(name, a2ui::spec::text(d), parsed.value);
    }
    std::printf("\n");
  } else if (compact) {
    std::printf("Packed spec documents (flash, no heap):\n");
    for (auto d : {Document::BasicCatalogV1, Document::CommonTypesV1_0}) {
      const a2ui::PackedDocument& p = *a2ui::spec::packed(d);
      std::printf("  %-22s %5u nodes, %6u B of node table\n",
                  d == Document::BasicCatalogV1 ? "basic catalog" : "common_types", p.node_count,
                  p.node_count * 8u);
    }
    std::printf("\n");
  }

  std::printf("Live heap after each step, and the peak during it (host, 64-bit), %s:\n",
              compact ? "compact catalog" : load ? "basic_catalog::load(false)" : "Json catalog as in 0.1.0");
  g_count.peak_bytes = g_count.live_bytes;
  const Counters base = g_count;
  g_mark = g_count;
  a2ui::CatalogPtr catalog;
  if (compact || load) {
    auto r = a2ui::basic_catalog::load(compact);
    if (!r.ok()) {
      std::fprintf(stderr, "catalog: %s\n", r.error().message.c_str());
      return 1;
    }
    catalog = r.value();
    step(compact ? "basic_catalog::load(true) (from_packed)" : "basic_catalog::load(false) (from_json)");
    if (load) {
      (void)a2ui::spec::common_types("1.0");
      step("spec::common_types(\"1.0\") (already cached)");
    }
  } else {
    auto doc = a2ui::spec::parsed(Document::BasicCatalogV1);
    step("spec::parsed(BasicCatalogV1) (cached)");
    (void)a2ui::spec::common_types("1.0");
    step("spec::common_types(\"1.0\") (cached)");
    auto r = a2ui::Catalog::from_json(*doc);
    if (!r.ok()) {
      std::fprintf(stderr, "catalog: %s\n", r.error().message.c_str());
      return 1;
    }
    catalog = r.value();
    step("Catalog::from_json");
    a2ui::basic_catalog::bind_functions(*catalog);
    step("bind_functions");
  }
  auto processor = std::make_unique<a2ui::MessageProcessor>(std::vector<a2ui::CatalogPtr>{catalog});
  step("MessageProcessor");
  const long long catalog_total = g_count.live_bytes - base.live_bytes;
  const long long catalog_blocks = g_count.live_blocks - base.live_blocks;

  {
    auto parsed = a2ui::Json::parse(card);
    const a2ui::Json& messages = parsed.value.is_object() ? parsed.value["messages"] : parsed.value;
    a2ui::Status st = processor->process_messages(messages);
    if (!st.ok()) {
      std::fprintf(stderr, "card: %s\n", st.error().message.c_str());
      return 1;
    }
  }
  step("process the card (core only, no widgets)");

  std::printf("\nCatalog and processor: %lld B in %lld blocks (host, 64-bit)\n", catalog_total, catalog_blocks);
  return 0;
}
