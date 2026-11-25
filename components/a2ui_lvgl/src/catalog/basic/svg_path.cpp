// Copyright 2026 The a2ui-lvgl Authors
// SPDX-License-Identifier: Apache-2.0

#include "svg_path.h"

#include <algorithm>
#include <cmath>

namespace a2ui_lvgl {
namespace basic {

namespace {

constexpr float kViewBox = 24.0f;
constexpr float kPi = 3.14159265358979f;
constexpr int kSubRows = 4;  // Vertical samples per pixel row.

struct Pt {
  float x = 0, y = 0;
};

/** A line segment in pixels, top to bottom, with its winding direction. */
struct Edge {
  float x0, y0, x1, y1;
  int8_t dir;
};

/** Flattens path segments into edges, in pixels. */
class EdgeList {
 public:
  explicit EdgeList(float scale) : scale_(scale) {}

  bool full() const { return edges_.size() >= kMaxSvgEdges; }
  std::vector<Edge>& edges() { return edges_; }

  void line(Pt a, Pt b) {
    if (full()) return;
    const float ax = a.x * scale_, ay = a.y * scale_, bx = b.x * scale_, by = b.y * scale_;
    if (ay == by) return;  // Horizontal edges cross no sample row.
    if (ay < by) {
      edges_.push_back({ax, ay, bx, by, 1});
    } else {
      edges_.push_back({bx, by, ax, ay, -1});
    }
  }

  /** Segments for a curve whose control polygon is `length` view-box units long. */
  int steps(float length) const {
    const int n = static_cast<int>(length * scale_ / 2.0f) + 1;  // About 2 px per segment.
    return n < 2 ? 2 : (n > 32 ? 32 : n);
  }

 private:
  float scale_;
  std::vector<Edge> edges_;
};

float dist(Pt a, Pt b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

class PathParser {
 public:
  PathParser(std::string_view s, EdgeList* out) : s_(s), out_(out) {}

  void run() {
    char cmd = 0;
    for (;;) {
      skip_ws();
      if (i_ >= s_.size() || out_->full()) break;
      const char c = s_[i_];
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        if (cmd == 0 && c != 'M' && c != 'm') return;  // Path data starts with a moveto.
        cmd = c;
        ++i_;
      } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
        break;  // Numbers without a command.
      } else if (cmd == 'M') {
        cmd = 'L';  // Pairs after a moveto are linetos.
      } else if (cmd == 'm') {
        cmd = 'l';
      }
      if (!command(cmd)) break;
    }
    close();
  }

 private:
  bool command(char cmd) {
    const bool rel = cmd >= 'a';
    const Pt o = rel ? cur_ : Pt{};
    float v[7];
    switch (cmd) {
      case 'M':
      case 'm':
        if (!numbers(v, 2)) return false;
        close();
        cur_ = start_ = {o.x + v[0], o.y + v[1]};
        break;
      case 'L':
      case 'l':
        if (!numbers(v, 2)) return false;
        line_to({o.x + v[0], o.y + v[1]});
        break;
      case 'H':
      case 'h':
        if (!numbers(v, 1)) return false;
        line_to({o.x + v[0], cur_.y});
        break;
      case 'V':
      case 'v':
        if (!numbers(v, 1)) return false;
        line_to({cur_.x, o.y + v[0]});
        break;
      case 'C':
      case 'c':
      case 'S':
      case 's': {
        const bool smooth = cmd == 'S' || cmd == 's';
        if (!numbers(v, smooth ? 4 : 6)) return false;
        Pt c1, c2, end;
        int k = 0;
        if (smooth) {
          c1 = last_ == 'C' ? Pt{2 * cur_.x - ctrl_.x, 2 * cur_.y - ctrl_.y} : cur_;
        } else {
          c1 = {o.x + v[0], o.y + v[1]};
          k = 2;
        }
        c2 = {o.x + v[k], o.y + v[k + 1]};
        end = {o.x + v[k + 2], o.y + v[k + 3]};
        cubic(c1, c2, end);
        ctrl_ = c2;
        last_ = 'C';
        return true;
      }
      case 'Q':
      case 'q':
      case 'T':
      case 't': {
        const bool smooth = cmd == 'T' || cmd == 't';
        if (!numbers(v, smooth ? 2 : 4)) return false;
        Pt c, end;
        if (smooth) {
          c = last_ == 'Q' ? Pt{2 * cur_.x - ctrl_.x, 2 * cur_.y - ctrl_.y} : cur_;
          end = {o.x + v[0], o.y + v[1]};
        } else {
          c = {o.x + v[0], o.y + v[1]};
          end = {o.x + v[2], o.y + v[3]};
        }
        quad(c, end);
        ctrl_ = c;
        last_ = 'Q';
        return true;
      }
      case 'A':
      case 'a': {
        bool large = false, sweep = false;
        if (!numbers(v, 3) || !flag(&large) || !flag(&sweep) || !numbers(v + 3, 2)) return false;
        arc(v[0], v[1], v[2], large, sweep, {o.x + v[3], o.y + v[4]});
        break;
      }
      case 'Z':
      case 'z':
        close();
        cur_ = start_;
        break;
      default:
        return false;
    }
    last_ = 0;
    return true;
  }

  void skip_ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\f')) {
      ++i_;
    }
  }

  /** Skips whitespace and at most one comma before an argument. */
  void skip_sep() {
    skip_ws();
    if (i_ < s_.size() && s_[i_] == ',') {
      ++i_;
      skip_ws();
    }
  }

  bool digit() const { return i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9'; }

  /** An SVG number: [sign] (digits [. digits] | . digits) [(e|E) [sign] digits]. */
  bool number(float* out) {
    skip_sep();
    double sign = 1, value = 0;
    if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
      if (s_[i_] == '-') sign = -1;
      ++i_;
    }
    bool any = false;
    while (digit()) {
      value = value * 10 + (s_[i_++] - '0');
      any = true;
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      ++i_;
      double scale = 0.1;
      while (digit()) {
        value += (s_[i_++] - '0') * scale;
        scale *= 0.1;
        any = true;
      }
    }
    if (!any) return false;
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      const size_t save = i_++;
      int esign = 1, exp = 0;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) esign = s_[i_++] == '-' ? -1 : 1;
      if (!digit()) {
        i_ = save;  // Not an exponent after all.
      } else {
        while (digit()) exp = std::min(exp * 10 + (s_[i_++] - '0'), 64);
        for (int k = 0; k < exp; ++k) value = esign > 0 ? value * 10 : value / 10;
      }
    }
    value *= sign;
    if (!(value > -1e6 && value < 1e6)) return false;  // Far outside any icon.
    *out = static_cast<float>(value);
    return true;
  }

  bool numbers(float* out, int n) {
    for (int k = 0; k < n; ++k) {
      if (!number(out + k)) return false;
    }
    return true;
  }

  /** An arc flag: a single '0' or '1', separators optional. */
  bool flag(bool* out) {
    skip_sep();
    if (i_ >= s_.size() || (s_[i_] != '0' && s_[i_] != '1')) return false;
    *out = s_[i_++] == '1';
    return true;
  }

  void line_to(Pt p) {
    out_->line(cur_, p);
    cur_ = p;
    open_ = true;
  }

  /** The fill closes every subpath. */
  void close() {
    if (open_) out_->line(cur_, start_);
    open_ = false;
  }

  void cubic(Pt c1, Pt c2, Pt end) {
    const Pt a = cur_;
    const int n = out_->steps(dist(a, c1) + dist(c1, c2) + dist(c2, end));
    for (int k = 1; k <= n; ++k) {
      const float t = static_cast<float>(k) / n, u = 1 - t;
      const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
      line_to({w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * end.x, w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * end.y});
    }
  }

  void quad(Pt c, Pt end) {
    const Pt a = cur_;
    const int n = out_->steps(dist(a, c) + dist(c, end));
    for (int k = 1; k <= n; ++k) {
      const float t = static_cast<float>(k) / n, u = 1 - t;
      line_to({u * u * a.x + 2 * u * t * c.x + t * t * end.x, u * u * a.y + 2 * u * t * c.y + t * t * end.y});
    }
  }

  /** Endpoint arc to center form (SVG 1.1 appendix F.6.5), then line segments. */
  void arc(float rx, float ry, float degrees, bool large, bool sweep, Pt end) {
    const Pt a = cur_;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0 || ry == 0 || (a.x == end.x && a.y == end.y)) {
      line_to(end);
      return;
    }
    const float phi = degrees * kPi / 180, cs = std::cos(phi), sn = std::sin(phi);
    const float dx = (a.x - end.x) / 2, dy = (a.y - end.y) / 2;
    const float x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
    const float lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
      rx *= std::sqrt(lambda);
      ry *= std::sqrt(lambda);
    }
    const float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float coef = den > 0 && num > 0 ? std::sqrt(num / den) : 0;
    if (large == sweep) coef = -coef;
    const float cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
    const float cx = cs * cxp - sn * cyp + (a.x + end.x) / 2, cy = sn * cxp + cs * cyp + (a.y + end.y) / 2;
    const float t1 = std::atan2((y1 - cyp) / ry, (x1 - cxp) / rx);
    float dt = std::atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx) - t1;
    if (sweep && dt < 0) dt += 2 * kPi;
    if (!sweep && dt > 0) dt -= 2 * kPi;
    const int n = out_->steps(std::fabs(dt) * std::max(rx, ry));
    for (int k = 1; k < n; ++k) {
      const float t = t1 + dt * k / n, ct = std::cos(t), st = std::sin(t);
      line_to({cx + rx * cs * ct - ry * sn * st, cy + rx * sn * ct + ry * cs * st});
    }
    line_to(end);  // Exactly on the endpoint.
  }

  std::string_view s_;
  EdgeList* out_;
  size_t i_ = 0;
  Pt cur_, start_, ctrl_;
  char last_ = 0;  // 'C' or 'Q' after a curve (for S and T reflection).
  bool open_ = false;
};

/** Adds the coverage of the span [xa, xb) to one row of accumulators. */
void add_span(float* acc, int size, float xa, float xb) {
  xa = std::max(xa, 0.0f);
  xb = std::min(xb, static_cast<float>(size));
  if (xb <= xa) return;
  const int ia = static_cast<int>(xa), ib = static_cast<int>(xb);
  if (ia == ib) {
    acc[ia] += xb - xa;
    return;
  }
  acc[ia] += ia + 1 - xa;
  for (int k = ia + 1; k < ib; ++k) acc[k] += 1;
  if (ib < size) acc[ib] += xb - ib;
}

struct Crossing {
  float x;
  int8_t dir;
};

}  // namespace

bool render_svg_path(std::string_view path, int size, std::vector<uint8_t>* out) {
  out->clear();
  if (size <= 0 || size > 512 || path.size() > kMaxSvgPathLength) return false;
  EdgeList edges(static_cast<float>(size) / kViewBox);
  PathParser(path, &edges).run();
  if (edges.edges().empty()) return false;

  out->assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
  std::vector<float> acc(static_cast<size_t>(size));
  std::vector<Crossing> xs;
  for (int y = 0; y < size; ++y) {
    std::fill(acc.begin(), acc.end(), 0.0f);
    for (int sub = 0; sub < kSubRows; ++sub) {
      const float sy = y + (sub + 0.5f) / kSubRows;
      xs.clear();
      for (const Edge& e : edges.edges()) {
        if (sy < e.y0 || sy >= e.y1) continue;
        xs.push_back({e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir});
      }
      // Insertion sort: a row crosses only a few edges.
      for (size_t k = 1; k < xs.size(); ++k) {
        const Crossing c = xs[k];
        size_t j = k;
        for (; j > 0 && xs[j - 1].x > c.x; --j) xs[j] = xs[j - 1];
        xs[j] = c;
      }
      int winding = 0;
      float from = 0;
      for (const Crossing& c : xs) {
        const int before = winding;
        winding += c.dir;
        if (before == 0 && winding != 0) {
          from = c.x;
        } else if (before != 0 && winding == 0) {
          add_span(acc.data(), size, from, c.x);
        }
      }
    }
    uint8_t* row = out->data() + static_cast<size_t>(y) * static_cast<size_t>(size);
    for (int x = 0; x < size; ++x) {
      const float a = acc[static_cast<size_t>(x)] * (255.0f / kSubRows) + 0.5f;
      row[x] = static_cast<uint8_t>(a > 255 ? 255 : a);
    }
  }
  return true;
}

}  // namespace basic
}  // namespace a2ui_lvgl
