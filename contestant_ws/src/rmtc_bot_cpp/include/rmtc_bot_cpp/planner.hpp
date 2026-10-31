#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rmtc_bot_cpp/types.hpp"

namespace rmtc {

// 网格路径规划：Dijkstra 最短路（可加危险代价）、方向换算、前沿探索。
// 未知格子按可通行乐观规划：每 Tick 只执行路径第一步并重新规划，
// 临近时视野会揭示真实障碍；撞墙/越界回执（E_BLOCKED/E_OOB）由 world 记忆纠正。

// 从 start 到相邻 target 的方向名；不相邻返回 nullopt。
std::optional<std::string> direction_to(const Cell& start, const Cell& target);

// 求 start 到 goal 的最短可行路径（不含 start，含 goal）。
// danger: 单元格 -> 附加代价（默认 0）；用于绕开敌方火力区。
// goal 本身被阻挡时视为不可达。地图最大 24×24（576 格），耗时可忽略。
std::optional<std::vector<Cell>> dijkstra(
    const Cell& start, const Cell& goal, int width, int height,
    const std::unordered_set<Cell>& blocked,
    const std::unordered_map<Cell, int>* danger = nullptr);

struct GoalResult {
  Cell target;
  std::vector<Cell> path;
};

// 在多个目标中选（价值权重 / 距离）最优者。goals: [(cell, value)]；
// weight 可选：cell -> 价值权重，缺省用 goals 中的 value。
// 目标按坐标排序后评分，评分相等取坐标最小者，保证逐 Tick 确定性。
// stick 可选：上一 Tick 选中的目标；若其仍可达且评分不低于最优的
// stick_margin（默认 0.8）倍，则保持该目标，避免近分值目标间来回切换。
// 所有目标均不可达时返回 nullopt。
std::optional<GoalResult> nearest_goal(
    const Cell& start, const std::vector<std::pair<Cell, int>>& goals, int width,
    int height, const std::unordered_set<Cell>& blocked,
    const std::unordered_map<Cell, int>* danger = nullptr,
    const std::unordered_map<Cell, int>* weight = nullptr,
    const Cell* stick = nullptr, double stick_margin = 0.8);

struct Frontier {
  Cell cell;
  std::vector<Cell> path;
  int novelty = 0;
};

// 探索前沿：可达且与未知格相邻的格子，按 (距离, -新信息量) 排序。
// known 为已知格集合（障碍 + 已确认空格的并集）。
std::vector<Frontier> frontier_cells(const Cell& start, int width, int height,
                                     const std::unordered_set<Cell>& blocked,
                                     const std::unordered_set<Cell>& known);

// 已知集合 = 障碍 ∪ 已访问格。
std::unordered_set<Cell> known_free_set(const std::unordered_set<Cell>& blocked,
                                        const std::unordered_set<Cell>& visited);

}  // namespace rmtc
