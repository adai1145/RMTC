#include "rmtc_bot_cpp/planner.hpp"

#include <algorithm>
#include <climits>
#include <queue>

#include "rmtc_bot_cpp/protocol.hpp"

namespace rmtc {

namespace {
constexpr Cell kNeighbors[4] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
}  // namespace

std::optional<std::string> direction_to(const Cell& start, const Cell& target) {
  return delta_dir(Cell{target.x - start.x, target.y - start.y});
}

std::optional<std::vector<Cell>> dijkstra(
    const Cell& start, const Cell& goal, int width, int height,
    const std::unordered_set<Cell>& blocked,
    const std::unordered_map<Cell, int>* danger) {
  if (!in_bounds(start, width, height) || !in_bounds(goal, width, height)) {
    return std::nullopt;
  }
  if (blocked.count(goal) > 0) {
    return std::nullopt;
  }
  const int n = width * height;
  const auto idx = [width](const Cell& c) { return c.y * width + c.x; };
  std::vector<int> dist(n, INT_MAX);
  std::vector<int> prev(n, -1);
  using Entry = std::pair<int, int>;  // (cost, cell_index)
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;

  const int start_idx = idx(start);
  const int goal_idx = idx(goal);
  dist[start_idx] = 0;
  queue.emplace(0, start_idx);

  while (!queue.empty()) {
    const auto [cost, current] = queue.top();
    queue.pop();
    if (cost > dist[current]) {
      continue;
    }
    if (current == goal_idx) {
      std::vector<Cell> path;
      int node = current;
      while (node != start_idx) {
        path.push_back(Cell{node % width, node / width});
        node = prev[node];
      }
      std::reverse(path.begin(), path.end());
      return path;
    }
    const Cell cur{current % width, current / width};
    for (const Cell& d : kNeighbors) {
      const Cell nxt{cur.x + d.x, cur.y + d.y};
      if (!in_bounds(nxt, width, height) || blocked.count(nxt) > 0) {
        continue;
      }
      int extra = 0;
      if (danger != nullptr) {
        const auto it = danger->find(nxt);
        if (it != danger->end()) {
          extra = it->second;
        }
      }
      const int total = cost + 1 + extra;
      const int nxt_idx = idx(nxt);
      if (total < dist[nxt_idx]) {
        dist[nxt_idx] = total;
        prev[nxt_idx] = current;
        queue.emplace(total, nxt_idx);
      }
    }
  }
  return std::nullopt;
}

std::optional<GoalResult> nearest_goal(
    const Cell& start, const std::vector<std::pair<Cell, int>>& goals, int width,
    int height, const std::unordered_set<Cell>& blocked,
    const std::unordered_map<Cell, int>* danger,
    const std::unordered_map<Cell, int>* weight, const Cell* stick,
    double stick_margin) {
  if (goals.empty()) {
    return std::nullopt;
  }
  // 排序保证遍历顺序确定，评分相等时稳定选坐标最小者。
  auto ordered = goals;
  std::sort(ordered.begin(), ordered.end(),
            [](const std::pair<Cell, int>& a, const std::pair<Cell, int>& b) {
              if (a.first.x != b.first.x) {
                return a.first.x < b.first.x;
              }
              return a.first.y < b.first.y;
            });
  std::optional<GoalResult> best;
  double best_score = -1.0;
  std::optional<GoalResult> stick_result;
  double stick_score = -1.0;
  for (const auto& [cell, value] : ordered) {
    auto path = dijkstra(start, cell, width, height, blocked, danger);
    if (!path.has_value()) {
      continue;
    }
    if (path->empty()) {
      continue;  // 目标即自身位置：无路可走，跳过（否则 w/0 得 +inf 恒压其他目标）
    }
    int w = value;
    if (weight != nullptr) {
      const auto it = weight->find(cell);
      if (it != weight->end()) {
        w = it->second;
      }
    }
    const double score = static_cast<double>(w) / path->size();
    if (score > best_score) {
      best_score = score;
      best = GoalResult{cell, *path};
    }
    if (stick != nullptr && cell == *stick) {
      stick_score = score;
      stick_result = GoalResult{cell, std::move(*path)};
    }
  }
  if (stick != nullptr && stick_result.has_value() && best.has_value() &&
      stick_score >= best_score * stick_margin) {
    return stick_result;
  }
  return best;
}

namespace {
int novelty_of(const Cell& cell, int width, int height,
               const std::unordered_set<Cell>& blocked,
               const std::unordered_set<Cell>& known, int radius = 4) {
  int count = 0;
  for (int dx = -radius; dx <= radius; ++dx) {
    for (int dy = -radius; dy <= radius; ++dy) {
      const Cell nxt{cell.x + dx, cell.y + dy};
      if (in_bounds(nxt, width, height) && known.count(nxt) == 0 &&
          blocked.count(nxt) == 0) {
        ++count;
      }
    }
  }
  return count;
}
}  // namespace

std::vector<Frontier> frontier_cells(const Cell& start, int width, int height,
                                     const std::unordered_set<Cell>& blocked,
                                     const std::unordered_set<Cell>& known) {
  // 单位权 BFS：未知格按可通行乐观遍历。
  std::unordered_map<Cell, int> dist;
  std::unordered_map<Cell, Cell> came_from;
  std::queue<Cell> queue;
  dist[start] = 0;
  queue.push(start);
  while (!queue.empty()) {
    const Cell current = queue.front();
    queue.pop();
    const int cost = dist[current];
    for (const Cell& d : kNeighbors) {
      const Cell nxt{current.x + d.x, current.y + d.y};
      if (!in_bounds(nxt, width, height) || blocked.count(nxt) > 0) {
        continue;
      }
      const auto it = dist.find(nxt);
      if (it != dist.end() && it->second <= cost + 1) {
        continue;
      }
      dist[nxt] = cost + 1;
      came_from[nxt] = current;
      queue.push(nxt);
    }
  }

  std::vector<Frontier> candidates;
  for (const auto& [cell, cost] : dist) {
    (void)cost;
    if (cell == start || known.count(cell) > 0) {
      continue;
    }
    bool adjacent_unknown = false;
    for (const Cell& d : kNeighbors) {
      const Cell nxt{cell.x + d.x, cell.y + d.y};
      if (in_bounds(nxt, width, height) && known.count(nxt) == 0 &&
          blocked.count(nxt) == 0) {
        adjacent_unknown = true;
        break;
      }
    }
    if (!adjacent_unknown) {
      continue;
    }
    std::vector<Cell> path;
    Cell node = cell;
    while (node != start) {
      path.push_back(node);
      node = came_from[node];
    }
    std::reverse(path.begin(), path.end());
    candidates.push_back(
        Frontier{cell, std::move(path),
                 novelty_of(cell, width, height, blocked, known)});
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Frontier& a, const Frontier& b) {
              if (a.path.size() != b.path.size()) {
                return a.path.size() < b.path.size();
              }
              if (a.novelty != b.novelty) {
                return a.novelty > b.novelty;
              }
              if (a.cell.x != b.cell.x) {
                return a.cell.x < b.cell.x;
              }
              return a.cell.y < b.cell.y;
            });
  return candidates;
}

std::unordered_set<Cell> known_free_set(const std::unordered_set<Cell>& blocked,
                                        const std::unordered_set<Cell>& visited) {
  std::unordered_set<Cell> result = blocked;
  result.insert(visited.begin(), visited.end());
  return result;
}

}  // namespace rmtc
