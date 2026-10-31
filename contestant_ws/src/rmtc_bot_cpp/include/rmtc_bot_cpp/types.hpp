#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace rmtc {

// 网格坐标：原点 [0,0] 在左上角，x 向右增大、y 向下增大。
struct Cell {
  int x = 0;
  int y = 0;

  bool operator==(const Cell& o) const { return x == o.x && y == o.y; }
  bool operator!=(const Cell& o) const { return !(*this == o); }
  bool operator<(const Cell& o) const {
    return x != o.x ? x < o.x : y < o.y;
  }
};

inline int chebyshev(const Cell& a, const Cell& b) {
  int dx = a.x - b.x;
  if (dx < 0) dx = -dx;
  int dy = a.y - b.y;
  if (dy < 0) dy = -dy;
  return dx > dy ? dx : dy;
}

inline bool in_bounds(const Cell& c, int width, int height) {
  return c.x >= 0 && c.x < width && c.y >= 0 && c.y < height;
}

}  // namespace rmtc

namespace std {
template <>
struct hash<rmtc::Cell> {
  size_t operator()(const rmtc::Cell& c) const noexcept {
    return static_cast<size_t>(static_cast<uint32_t>(c.x)) * 1000003u ^
           static_cast<size_t>(static_cast<uint32_t>(c.y));
  }
};
}  // namespace std
