// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// Desktop simulator: shows A2UI surfaces in an SDL window through a2ui_lvgl,
// so you can click and type as on a device. Messages for the agent (actions,
// function calls, errors) are printed to stdout, one JSON object per line.
//
//   a2ui_simulator [messages.json] [--stdin] [--size WxH] [--dark]
//                  [--exit-after MS] [--click X,Y]... [--type TEXT] [--screenshot out.png]
//
// The file is an upstream example ({"messages": [...]}) or a bare message
// array, as for render_sample. With --stdin, each further line on stdin is
// one message (or an array of messages) from the agent, so the simulator can
// sit between pipes to and from an agent process. Diagnostics go to stderr.
//
// The newest surface is shown; when it is deleted, the previous one returns.
// --exit-after, --click, --type and --screenshot are for scripted runs,
// e.g. with SDL_VIDEODRIVER=dummy on a machine without a display. Clicks
// start 300 ms after launch and are 300 ms apart; --type follows the clicks.

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
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
#include "lvgl.h"
#include "png_writer.h"

#include LV_SDL_INCLUDE_PATH  // After lvgl.h, which defines it (lv_conf.h).

namespace {

volatile bool g_quit = false;

// Takes window-close and quit events (including Ctrl+C) away from LVGL's SDL
// driver, which would delete the display under the open surfaces.
int quit_filter(void*, SDL_Event* e) {
  if (e->type == SDL_QUIT || (e->type == SDL_WINDOWEVENT && e->window.event == SDL_WINDOWEVENT_CLOSE)) {
    g_quit = true;
    return 0;
  }
  return 1;
}

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
  std::fprintf(stderr,
               "usage: a2ui_simulator [messages.json] [--stdin] [--size WxH] [--dark]\n"
               "                      [--exit-after MS] [--click X,Y]... [--type TEXT] [--screenshot out.png]\n");
  return 2;
}

bool parse_pair(const std::string& v, char sep, int32_t* a, int32_t* b) {
  const size_t p = v.find(sep);
  if (p == std::string::npos) return false;
  char* end = nullptr;
  *a = static_cast<int32_t>(std::strtol(v.c_str(), &end, 10));
  if (end != v.c_str() + p) return false;
  *b = static_cast<int32_t>(std::strtol(v.c_str() + p + 1, &end, 10));
  return *end == '\0';
}

// The surfaces on screen, oldest first; only the last one is visible.
class Views {
 public:
  Views(a2ui::MessageProcessor& processor, const a2ui_lvgl::SurfaceConfig& config)
      : config_(config) {
    subs_.add(processor.model().on_surface_created().subscribe([this](a2ui::SurfaceModel* s) { add(*s); }));
    subs_.add(processor.model().on_surface_deleted().subscribe([this](const std::string& id) { remove(id); }));
  }

  void clear() {
    subs_.clear();
    views_.clear();
  }

 private:
  struct View {
    std::string id;
    a2ui::Subscription errors;
    std::unique_ptr<a2ui_lvgl::Surface> surface;
  };

  void add(a2ui::SurfaceModel& model) {
    for (auto& v : views_) {
      if (v.surface->obj()) lv_obj_set_hidden(v.surface->obj(), true);
    }
    View v;
    v.id = model.id();
    v.errors = model.on_error().subscribe(
        [](const a2ui::Json& e) { std::fprintf(stderr, "surface error: %s\n", e.dump().c_str()); });
    v.surface = std::make_unique<a2ui_lvgl::Surface>(model, lv_screen_active(), config_);
    std::fprintf(stderr, "showing surface %s\n", v.id.c_str());
    views_.push_back(std::move(v));
  }

  void remove(const std::string& id) {
    views_.erase(std::remove_if(views_.begin(), views_.end(), [&](const View& v) { return v.id == id; }),
                 views_.end());
    if (!views_.empty() && views_.back().surface->obj()) {
      lv_obj_set_hidden(views_.back().surface->obj(), false);
      std::fprintf(stderr, "showing surface %s\n", views_.back().id.c_str());
    }
  }

  a2ui_lvgl::SurfaceConfig config_;
  a2ui::SubscriptionBag subs_;
  std::vector<View> views_;
};

// Reads whole lines from a non-blocking stdin and hands each to the processor.
class StdinReader {
 public:
  explicit StdinReader(a2ui::MessageProcessor& processor) : processor_(processor) {}

  bool open() const { return open_; }

  /** Waits up to timeout_ms for input, then processes every complete line. */
  void pump(int timeout_ms) {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) return;
    char buf[4096];
    const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n <= 0) {
      open_ = false;
      if (!pending_.empty()) handle(pending_);
      pending_.clear();
      return;
    }
    pending_.append(buf, static_cast<size_t>(n));
    size_t start = 0, nl;
    while ((nl = pending_.find('\n', start)) != std::string::npos) {
      handle(pending_.substr(start, nl - start));
      start = nl + 1;
    }
    pending_.erase(0, start);
  }

 private:
  void handle(const std::string& line) {
    if (line.find_first_not_of(" \t\r") == std::string::npos) return;
    a2ui::Status st = processor_.process_text(line);
    if (!st.ok()) std::fprintf(stderr, "stdin: %s\n", st.error().message.c_str());
  }

  a2ui::MessageProcessor& processor_;
  std::string pending_;
  bool open_ = true;
};

void push_mouse(lv_display_t* disp, int32_t x, int32_t y, bool down) {
  SDL_Event e{};
  const uint32_t window = SDL_GetWindowID(lv_sdl_window_get_window(disp));
  e.type = SDL_MOUSEMOTION;
  e.motion.windowID = window;
  e.motion.x = x;
  e.motion.y = y;
  SDL_PushEvent(&e);
  e = SDL_Event{};
  e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
  e.button.windowID = window;
  e.button.button = SDL_BUTTON_LEFT;
  e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
  e.button.x = x;
  e.button.y = y;
  SDL_PushEvent(&e);
}

// Types `text` (ASCII) as if from the keyboard, into the focused text field.
void push_text(lv_display_t* disp, const std::string& text) {
  for (char c : text) {
    SDL_Event e{};
    e.type = SDL_TEXTINPUT;
    e.text.windowID = SDL_GetWindowID(lv_sdl_window_get_window(disp));
    e.text.text[0] = c;
    SDL_PushEvent(&e);
  }
}

bool write_screenshot(lv_display_t* disp, const std::string& path) {
  lv_refr_now(disp);
  lv_draw_buf_t* buf = lv_display_get_buf_active(disp);
  if (!buf || buf->header.cf != LV_COLOR_FORMAT_XRGB8888) {
    std::fprintf(stderr, "screenshot: expected an XRGB8888 frame buffer\n");
    return false;
  }
  const uint32_t w = buf->header.w, h = buf->header.h;
  std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = buf->data + static_cast<size_t>(y) * buf->header.stride;
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* px = &rgb[(static_cast<size_t>(y) * w + x) * 3];
      px[0] = row[x * 4 + 2];  // XRGB8888 is stored B, G, R, X.
      px[1] = row[x * 4 + 1];
      px[2] = row[x * 4 + 0];
    }
  }
  return png_writer::write_rgb(path, w, h, rgb);
}

}  // namespace

int main(int argc, char** argv) {
  std::string in, screenshot, type;
  int32_t width = 480, height = 800;
  bool dark = false, use_stdin = false;
  long exit_after = 0;
  struct Click {
    int32_t x, y;
  };
  std::vector<Click> clicks;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dark") {
      dark = true;
    } else if (a == "--stdin") {
      use_stdin = true;
    } else if (a == "--size" && i + 1 < argc) {
      if (!parse_pair(argv[++i], 'x', &width, &height) || width <= 0 || height <= 0) return usage();
    } else if (a == "--exit-after" && i + 1 < argc) {
      exit_after = std::strtol(argv[++i], nullptr, 10);
      if (exit_after <= 0) return usage();
    } else if (a == "--click" && i + 1 < argc) {
      Click c{};
      if (!parse_pair(argv[++i], ',', &c.x, &c.y)) return usage();
      clicks.push_back(c);
    } else if (a == "--type" && i + 1 < argc) {
      type = argv[++i];
    } else if (a == "--screenshot" && i + 1 < argc) {
      screenshot = argv[++i];
    } else if (!a.empty() && a[0] != '-' && in.empty()) {
      in = a;
    } else {
      return usage();
    }
  }
  if (in.empty() && !use_stdin) return usage();

  a2ui::Json messages;
  if (!in.empty()) {
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
    messages = parsed.value.is_object() ? parsed.value["messages"] : parsed.value;
  }

  auto cat = a2ui::Catalog::from_json(*a2ui::spec::parsed(a2ui::spec::Document::BasicCatalogV1));
  if (!cat.ok()) {
    std::fprintf(stderr, "basic catalog: %s\n", cat.error().message.c_str());
    return 1;
  }
  a2ui::basic_catalog::bind_functions(*cat.value());

  lv_init();
  lv_log_register_print_cb([](lv_log_level_t, const char* line) { std::fputs(line, stderr); });
  lv_display_t* disp = lv_sdl_window_create(width, height);
  if (!disp) {
    std::fprintf(stderr, "cannot open an SDL window: %s\n", SDL_GetError());
    lv_deinit();
    return 1;
  }
  SDL_SetEventFilter(quit_filter, nullptr);
  lv_sdl_window_set_title(disp, in.empty() ? "A2UI simulator" : ("A2UI simulator - " + in).c_str());

  // Clicking a text field focuses it in the group; the keyboard types into it
  // and Tab moves between fields.
  lv_group_t* group = lv_group_create();
  lv_group_set_default(group);
  lv_sdl_mouse_create();
  lv_indev_set_group(lv_sdl_keyboard_create(), group);

  const a2ui_lvgl::Theme theme = dark ? a2ui_lvgl::Theme::dark_theme() : a2ui_lvgl::Theme::light();
  a2ui_lvgl::apply_lvgl_theme(disp, theme);
  lv_obj_set_style_bg_color(lv_screen_active(), theme.background, 0);

  int status = 0;
  {
    a2ui::MessageProcessorOptions options;
    options.forward_actions = true;
    options.outbound_listener = [](const a2ui::Json& m) {
      std::fputs(m.dump().c_str(), stdout);
      std::fputc('\n', stdout);
      std::fflush(stdout);
    };
    a2ui::MessageProcessor processor({cat.value()}, std::move(options));

    a2ui_lvgl::SurfaceConfig config;
    config.registry = &a2ui_lvgl::basic_registry();
    config.theme = &theme;
    Views views(processor, config);

    if (!in.empty()) {
      a2ui::Status st = processor.process_messages(messages);
      if (!st.ok()) std::fprintf(stderr, "%s: %s\n", in.c_str(), st.error().message.c_str());
    }

    StdinReader reader(processor);
    size_t next_click = 0;
    bool pressed = false;
    const uint32_t start = lv_tick_get();
    uint32_t last_click = start;
    while (!g_quit && (exit_after == 0 || lv_tick_elaps(start) < static_cast<uint32_t>(exit_after))) {
      processor.tick(lv_tick_get());
      const uint32_t idle = std::min<uint32_t>(lv_timer_handler(), 10);
      // Scripted clicks: press, hold 100 ms so the mouse driver reads it, release.
      if (next_click < clicks.size() && lv_tick_elaps(last_click) >= (pressed ? 100u : 300u)) {
        push_mouse(disp, clicks[next_click].x, clicks[next_click].y, !pressed);
        if (pressed) ++next_click;
        pressed = !pressed;
        last_click = lv_tick_get();
      } else if (next_click == clicks.size() && !type.empty() && lv_tick_elaps(last_click) >= 300u) {
        push_text(disp, type);  // After the clicks, which may have focused a field.
        type.clear();
      }
      if (use_stdin && reader.open()) {
        reader.pump(static_cast<int>(idle));
      } else {
        SDL_Delay(idle);
      }
    }

    if (!screenshot.empty()) {
      if (write_screenshot(disp, screenshot)) {
        std::fprintf(stderr, "wrote %s\n", screenshot.c_str());
      } else {
        std::fprintf(stderr, "cannot write %s\n", screenshot.c_str());
        status = 1;
      }
    }
    views.clear();
    processor.dispose();
  }
  lv_display_delete(disp);
  lv_sdl_quit();
  lv_deinit();
  return status;
}
