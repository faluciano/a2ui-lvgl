// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Renders an A2UI message file through a2ui_lvgl on a headless LVGL display
// and writes the frame as a PNG, so humans can look at adapter output.
//
//   render_sample <example.json> <out.png> [--size 480x800] [--dark]
//
// The input is an upstream example ({"messages": [...]}) or a bare message
// array. Images show their placeholder boxes (no image loader).

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "a2ui/basic_catalog.h"
#include "a2ui/catalog.h"
#include "a2ui/message_processor.h"
#include "a2ui/spec_documents.h"
#include "a2ui_lvgl/a2ui_lvgl.h"
#include "lvgl_env.h"
#include "png_writer.h"

namespace {

std::string read_file(const std::string& path, bool* ok) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  *ok = f != nullptr;
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

int usage() {
  std::fprintf(stderr, "usage: render_sample <example.json> <out.png> [--size WxH] [--dark]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string in = argv[1];
  const std::string out = argv[2];
  int32_t width = 480, height = 800;
  bool dark = false;
  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--dark") {
      dark = true;
    } else if (a == "--size" && i + 1 < argc) {
      const std::string v = argv[++i];
      const size_t x = v.find('x');
      if (x == std::string::npos) return usage();
      width = std::atoi(v.substr(0, x).c_str());
      height = std::atoi(v.substr(x + 1).c_str());
      if (width <= 0 || height <= 0) return usage();
    } else {
      return usage();
    }
  }

  bool ok = false;
  const std::string text = read_file(in, &ok);
  if (!ok) {
    std::fprintf(stderr, "cannot read %s\n", in.c_str());
    return 1;
  }
  a2ui::Json::ParseResult parsed = a2ui::Json::parse(text);
  if (!parsed.ok) {
    std::fprintf(stderr, "%s: invalid JSON at %zu: %s\n", in.c_str(), parsed.offset, parsed.error.c_str());
    return 1;
  }
  a2ui::Json messages = parsed.value.is_object() ? parsed.value["messages"] : parsed.value;

  lvgl_env::init();
  {
    lvgl_env::Display disp;
    lvgl_env::create_display(&disp, width, height, LV_COLOR_FORMAT_XRGB8888, true);
    const a2ui_lvgl::Theme theme = dark ? a2ui_lvgl::Theme::dark_theme() : a2ui_lvgl::Theme::light();
    a2ui_lvgl::apply_lvgl_theme(disp.display, theme);
    lv_obj_set_style_bg_color(lv_screen_active(), theme.background, 0);

    auto cat = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
    if (!cat.ok()) {
      std::fprintf(stderr, "basic catalog: %s\n", cat.error().message.c_str());
      return 1;
    }
    a2ui::basic_catalog::bind_functions(*cat.value());
    a2ui::MessageProcessorOptions options;
    options.outbound_listener = [](const a2ui::Json& m) { std::printf("outbound: %s\n", m.dump().c_str()); };
    a2ui::MessageProcessor processor({cat.value()}, options);
    a2ui::Status st = processor.process_messages(messages);
    if (!st.ok()) std::fprintf(stderr, "warning: %s\n", st.error().message.c_str());

    std::vector<std::unique_ptr<a2ui_lvgl::Surface>> views;
    a2ui_lvgl::SurfaceConfig config;
    config.registry = &a2ui_lvgl::basic_registry();
    config.theme = &theme;
    for (const auto& s : processor.model().surfaces()) {
      views.push_back(std::make_unique<a2ui_lvgl::Surface>(*s.second, lv_screen_active(), config));
      break;  // One surface per frame.
    }
    if (views.empty()) {
      std::fprintf(stderr, "%s: no surface\n", in.c_str());
      return 1;
    }
    lvgl_env::run(5);
    lv_obj_invalidate(lv_screen_active());
    lvgl_env::refresh();

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
    for (int32_t y = 0; y < height; ++y) {
      const uint8_t* row = disp.pixels.data() + static_cast<size_t>(y) * disp.stride;
      for (int32_t x = 0; x < width; ++x) {
        uint8_t* px = &rgb[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3];
        px[0] = row[x * 4 + 2];  // XRGB8888 is stored B, G, R, X.
        px[1] = row[x * 4 + 1];
        px[2] = row[x * 4 + 0];
      }
    }
    if (!png_writer::write_rgb(out, static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgb)) {
      std::fprintf(stderr, "cannot write %s\n", out.c_str());
      return 1;
    }
    std::printf("%s -> %s (%dx%d)\n", in.c_str(), out.c_str(), static_cast<int>(width), static_cast<int>(height));
    views.clear();
    processor.dispose();
    lv_display_delete(disp.display);
  }
  lvgl_env::deinit();
  return 0;
}
