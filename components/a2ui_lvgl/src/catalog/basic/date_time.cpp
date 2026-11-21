// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0
//
// DateTimeInput: a field showing the value; tapping it opens a picker on the
// top layer (lv_calendar for the date, hour/minute rollers for the time)
// with Cancel / OK. OK writes an ISO 8601 string (iso8601.h) through the
// value binding. min/max are not enforced by the picker (lv_calendar has no
// range); checks on the component still validate the value.

#include "iso8601.h"

#include <cstdlib>

#include "basic_components.h"

namespace a2ui_lvgl {
namespace basic {

// --- ISO 8601 ------------------------------------------------------------------

namespace {

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool read_digits(const std::string& s, size_t pos, size_t n, int* out) {
  if (pos + n > s.size()) return false;
  int v = 0;
  for (size_t i = 0; i < n; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  *out = v;
  return true;
}

bool parse_date(const std::string& s, IsoDateTime* out) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  int y = 0, m = 0, d = 0;
  if (!read_digits(s, 0, 4, &y) || !read_digits(s, 5, 2, &m) || !read_digits(s, 8, 2, &d)) return false;
  if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return false;
  out->has_date = true;
  out->year = y;
  out->month = m;
  out->day = d;
  return true;
}

bool parse_time(const std::string& s, bool allow_fraction, IsoDateTime* out) {
  int h = 0, mi = 0, sec = 0;
  if (s.size() < 5 || s[2] != ':' || !read_digits(s, 0, 2, &h) || !read_digits(s, 3, 2, &mi)) return false;
  size_t pos = 5;
  bool seconds = false;
  std::string fraction;
  if (pos < s.size()) {
    if (s[pos] != ':' || !read_digits(s, pos + 1, 2, &sec)) return false;
    seconds = true;
    pos += 3;
    if (pos < s.size()) {
      if (!allow_fraction || s[pos] != '.') return false;
      ++pos;
      const size_t start = pos;
      while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
      if (pos == start || pos != s.size()) return false;
      fraction = s.substr(start);
    }
  }
  if (h > 23 || mi > 59 || sec > 59) return false;
  out->has_time = true;
  out->hour = h;
  out->minute = mi;
  out->second = sec;
  out->has_seconds = seconds;
  out->fraction = fraction;
  return true;
}

std::string zone_suffix(const std::string& t) {
  if (!t.empty() && (t.back() == 'Z' || t.back() == 'z')) return "Z";
  for (size_t len : {6, 5, 3}) {
    if (t.size() <= len) continue;
    const std::string z = t.substr(t.size() - len);
    if (z[0] != '+' && z[0] != '-') continue;
    int hh = 0, mm = 0;
    if (!read_digits(z, 1, 2, &hh) || hh > 23) continue;
    if (len == 6 && (z[3] != ':' || !read_digits(z, 4, 2, &mm) || mm > 59)) continue;
    if (len == 5 && (!read_digits(z, 3, 2, &mm) || mm > 59)) continue;
    return z;
  }
  return {};
}

void append2(std::string* out, int v) {
  if (v < 0) v = 0;
  out->push_back(static_cast<char>('0' + (v / 10) % 10));
  out->push_back(static_cast<char>('0' + v % 10));
}

void append_date(std::string* out, const IsoDateTime& v) {
  int y = v.year < 0 ? 0 : (v.year > 9999 ? 9999 : v.year);
  out->push_back(static_cast<char>('0' + y / 1000));
  out->push_back(static_cast<char>('0' + (y / 100) % 10));
  append2(out, y % 100);
  out->push_back('-');
  append2(out, v.month);
  out->push_back('-');
  append2(out, v.day);
}

void append_time(std::string* out, const IsoDateTime& v, bool seconds) {
  append2(out, v.hour);
  out->push_back(':');
  append2(out, v.minute);
  if (seconds) {
    out->push_back(':');
    append2(out, v.second);
  }
}

}  // namespace

int days_in_month(int year, int month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && is_leap(year) ? 29 : kDays[month - 1];
}

bool parse_iso8601(const std::string& text, IsoDateTime* out) {
  IsoDateTime v;
  const size_t t = text.find_first_of("Tt");
  if (t != std::string::npos) {
    if (!parse_date(text.substr(0, t), &v)) return false;
    std::string rest = text.substr(t + 1);
    v.zone = zone_suffix(rest);
    rest.resize(rest.size() - v.zone.size());
    if (!parse_time(rest, true, &v)) return false;
    if (!v.fraction.empty() && !v.has_seconds) return false;
  } else if (text.size() == 10) {
    if (!parse_date(text, &v)) return false;
  } else if (!parse_time(text, false, &v)) {
    return false;
  }
  *out = v;
  return true;
}

std::string format_iso8601(const IsoDateTime& value, bool enable_date, bool enable_time,
                           const IsoDateTime* previous) {
  std::string out;
  if (enable_date && enable_time) {
    bool seconds = true;
    std::string zone;
    if (previous && previous->has_date && previous->has_time) {
      seconds = previous->has_seconds;
      zone = previous->zone;
    }
    append_date(&out, value);
    out.push_back('T');
    append_time(&out, value, seconds);
    out += zone;
  } else if (enable_time) {
    const bool seconds = previous && previous->has_time ? previous->has_seconds : true;
    append_time(&out, value, seconds);
  } else {
    append_date(&out, value);
  }
  return out;
}

// --- Widget ------------------------------------------------------------------------

namespace {

using a2ui::Json;

const char* hour_options() {
  static std::string opts;
  if (opts.empty()) {
    for (int h = 0; h < 24; ++h) {
      if (h) opts.push_back('\n');
      append2(&opts, h);
    }
  }
  return opts.c_str();
}

const char* minute_options() {
  static std::string opts;
  if (opts.empty()) {
    for (int m = 0; m < 60; ++m) {
      if (m) opts.push_back('\n');
      append2(&opts, m);
    }
  }
  return opts.c_str();
}

class DateTimeInput : public ComponentInstance {
 public:
  lv_obj_t* mount(NodeView& view, lv_obj_t* parent) override {
    view_ = &view;
    const Styles& s = styles(view);
    root_ = make_container(parent, s);
    lv_obj_add_style(root_, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(root_, lv_pct(100));
    lv_obj_set_style_pad_row(root_, 2, 0);
    label_ = lv_label_create(root_);
    lv_obj_add_style(label_, const_cast<lv_style_t*>(&s.caption), 0);
    lv_label_set_text_static(label_, "");
    field_ = lv_button_create(root_);
    lv_obj_remove_style_all(field_);
    lv_obj_add_style(field_, const_cast<lv_style_t*>(&s.field), 0);
    lv_obj_add_style(field_, const_cast<lv_style_t*>(&s.pressed), LV_STATE_PRESSED);
    lv_obj_set_size(field_, lv_pct(100), LV_SIZE_CONTENT);
    field_text_ = lv_label_create(field_);
    lv_label_set_text_static(field_text_, "");
    add_event<DateTimeInput, &DateTimeInput::on_field>(field_, LV_EVENT_CLICKED, this);
    feedback_ = lv_label_create(root_);
    lv_label_set_long_mode(feedback_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(feedback_, lv_pct(100));
    lv_label_set_text_static(feedback_, "");
    lv_obj_set_hidden(feedback_, true);
    return root_;
  }

  void unmount(NodeView&) override { popup_.close(); }

  void update(NodeView& view) override {
    const std::string label = view.string_prop("label");
    set_label_text(label_, label);
    if (label.empty()) {
      lv_obj_set_hidden(label_, true);
    } else {
      lv_obj_set_hidden(label_, false);
    }
    enable_date_ = view.bool_prop("enableDate", false);
    enable_time_ = view.bool_prop("enableTime", false);
    if (!enable_date_ && !enable_time_) enable_date_ = true;
    value_ = view.string_prop("value");
    parsed_ok_ = parse_iso8601(value_, &parsed_);
    std::string shown;
    if (parsed_ok_) {
      if (parsed_.has_date && enable_date_) shown = value_.substr(0, 10);
      if (parsed_.has_time && enable_time_) {
        if (!shown.empty()) shown.push_back(' ');
        std::string t;
        append_time(&t, parsed_, false);
        shown += t;
      }
    }
    if (shown.empty()) shown = value_.empty() ? (enable_date_ ? "Select date" : "Select time") : value_;
    set_label_text(field_text_, shown);
    lv_obj_set_style_text_color(field_text_, value_.empty() ? view.theme().muted : view.theme().on_background, 0);
    show_feedback(feedback_, check_feedback(view), styles(view));
  }

  void describe(const NodeView&, Json* out) const override {
    out->set("value", Json(value_));
    out->set("label", Json(lv_label_get_text(label_)));
    out->set("open", Json(popup_.is_open()));
    describe_feedback(feedback_, out);
  }

  void infer_accessibility(const NodeView& view, Accessibility* out) const override {
    out->role = "textbox";
    out->label = view.string_prop("label");
  }

  bool simulate_input(NodeView&, const Json& value) override {
    IsoDateTime want;
    if (!value.is_string() || !parse_iso8601(value.as_string(), &want)) return false;
    open_picker();
    if (want.has_date) select_date(want.year, want.month, want.day);
    if (want.has_time && hour_roller_) {
      lv_roller_set_selected(hour_roller_, static_cast<uint32_t>(want.hour), LV_ANIM_OFF);
      lv_roller_set_selected(minute_roller_, static_cast<uint32_t>(want.minute), LV_ANIM_OFF);
      pick_.second = want.second;
    }
    lv_obj_send_event(ok_, LV_EVENT_CLICKED, nullptr);
    return true;
  }

 private:
  void on_field(lv_event_t*) { open_picker(); }

  void open_picker() {
    if (popup_.is_open()) return;
    const Styles& s = styles(*view_);
    pick_ = parsed_ok_ ? parsed_ : IsoDateTime();
    if (!pick_.has_date) {
      pick_.year = 2026;
      pick_.month = 1;
      pick_.day = 1;
    }
    lv_obj_t* panel = popup_.open(root_, s, &DateTimeInput::dismiss, this);
    calendar_ = hour_roller_ = minute_roller_ = nullptr;
    if (enable_date_) {
      calendar_ = lv_calendar_create(panel);
      lv_obj_set_size(calendar_, lv_pct(100), 230);
#if LV_USE_CALENDAR_HEADER_ARROW
      lv_calendar_add_header_arrow(calendar_);
#endif
      lv_calendar_set_today_date(calendar_, static_cast<uint32_t>(pick_.year), static_cast<uint32_t>(pick_.month),
                                 static_cast<uint32_t>(pick_.day));
      select_date(pick_.year, pick_.month, pick_.day);
      add_event<DateTimeInput, &DateTimeInput::on_calendar>(calendar_, LV_EVENT_VALUE_CHANGED, this);
    }
    if (enable_time_) {
      lv_obj_t* row = make_container(panel, s);
      lv_obj_set_width(row, lv_pct(100));
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_ver(row, s.theme.leaf_margin, 0);
      hour_roller_ = lv_roller_create(row);
      lv_roller_set_options(hour_roller_, hour_options(), LV_ROLLER_MODE_NORMAL);
      lv_roller_set_visible_row_count(hour_roller_, 3);
      lv_roller_set_selected(hour_roller_, static_cast<uint32_t>(pick_.hour), LV_ANIM_OFF);
      lv_obj_t* colon = lv_label_create(row);
      lv_label_set_text_static(colon, ":");
      lv_obj_set_style_pad_hor(colon, s.theme.leaf_margin, 0);
      minute_roller_ = lv_roller_create(row);
      lv_roller_set_options(minute_roller_, minute_options(), LV_ROLLER_MODE_NORMAL);
      lv_roller_set_visible_row_count(minute_roller_, 3);
      lv_roller_set_selected(minute_roller_, static_cast<uint32_t>(pick_.minute), LV_ANIM_OFF);
    }
    lv_obj_t* buttons = make_container(panel, s);
    lv_obj_set_width(buttons, lv_pct(100));
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* cancel = make_button(buttons, s, "Cancel", false);
    add_event<DateTimeInput, &DateTimeInput::on_cancel>(cancel, LV_EVENT_CLICKED, this);
    ok_ = make_button(buttons, s, "OK", true);
    add_event<DateTimeInput, &DateTimeInput::on_ok>(ok_, LV_EVENT_CLICKED, this);
  }

  static lv_obj_t* make_button(lv_obj_t* parent, const Styles& s, const char* text, bool primary) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&s.button), 0);
    lv_obj_add_style(b, const_cast<lv_style_t*>(primary ? &s.button_primary : &s.button_borderless), 0);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&s.leaf), 0);
    lv_obj_add_style(b, const_cast<lv_style_t*>(&s.pressed), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, s.theme.leaf_margin * 2, 0);
    lv_obj_set_style_pad_ver(b, s.theme.leaf_margin, 0);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text_static(l, text);
    return b;
  }

  void select_date(int y, int m, int d) {
    pick_.year = y;
    pick_.month = m;
    pick_.day = d;
    pick_.has_date = true;
    if (!calendar_) return;
    highlighted_.year = static_cast<uint16_t>(y);
    highlighted_.month = static_cast<uint8_t>(m);
    highlighted_.day = static_cast<uint8_t>(d);
    lv_calendar_set_month_shown(calendar_, static_cast<uint32_t>(y), static_cast<uint32_t>(m));
    lv_calendar_set_highlighted_dates(calendar_, &highlighted_, 1);
  }

  void on_calendar(lv_event_t*) {
    lv_calendar_date_t date;
    if (lv_calendar_get_pressed_date(calendar_, &date) == LV_RESULT_OK) select_date(date.year, date.month, date.day);
  }

  static void dismiss(void* self) { static_cast<DateTimeInput*>(self)->popup_.close(); }
  void on_cancel(lv_event_t*) { popup_.close(); }

  void on_ok(lv_event_t*) {
    IsoDateTime v = pick_;
    if (hour_roller_) {
      v.hour = static_cast<int>(lv_roller_get_selected(hour_roller_));
      v.minute = static_cast<int>(lv_roller_get_selected(minute_roller_));
      v.has_time = true;
    }
    const std::string text = format_iso8601(v, enable_date_, enable_time_, parsed_ok_ ? &parsed_ : nullptr);
    popup_.close();
    calendar_ = hour_roller_ = minute_roller_ = ok_ = nullptr;
    a2ui::NodePtr node = view_->node_ptr();
    node->write("value", Json(text));
  }

  NodeView* view_ = nullptr;
  lv_obj_t* root_ = nullptr;
  lv_obj_t* label_ = nullptr;
  lv_obj_t* field_ = nullptr;
  lv_obj_t* field_text_ = nullptr;
  lv_obj_t* feedback_ = nullptr;
  lv_obj_t* calendar_ = nullptr;
  lv_obj_t* hour_roller_ = nullptr;
  lv_obj_t* minute_roller_ = nullptr;
  lv_obj_t* ok_ = nullptr;
  lv_calendar_date_t highlighted_{};
  Popup popup_;
  bool enable_date_ = true;
  bool enable_time_ = false;
  std::string value_;
  IsoDateTime parsed_;
  bool parsed_ok_ = false;
  IsoDateTime pick_;
};

}  // namespace

std::unique_ptr<ComponentInstance> make_date_time_input() { return std::make_unique<DateTimeInput>(); }

}  // namespace basic
}  // namespace a2ui_lvgl
