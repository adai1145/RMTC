#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "rmtc_bot_cpp/types.hpp"

namespace rmtc {

constexpr int MAX_ACTION_BYTES = 4096;
constexpr int VIEW_SIZE = 9;
constexpr int VIEW_CENTER = 4;  // 0 起编号的中心行/列（第 5 行第 5 列）

constexpr std::string_view ACTIONS[] = {"move", "attack", "defend", "wait"};
constexpr std::string_view DIRECTIONS[] = {"up", "down", "left", "right"};

inline bool is_action(const std::string& s) {
  for (const auto& a : ACTIONS) {
    if (s == a) return true;
  }
  return false;
}

inline bool is_direction(const std::string& s) {
  for (const auto& d : DIRECTIONS) {
    if (s == d) return true;
  }
  return false;
}

// 方向到坐标增量。
Cell dir_delta(const std::string& direction);
std::optional<std::string> delta_dir(const Cell& delta);
std::optional<std::string> opposite(const std::string& direction);

// 协议消息解析失败。调用方应丢弃该帧并记录日志，进程不得退出。
class ProtocolError : public std::runtime_error {
 public:
  explicit ProtocolError(const std::string& msg) : std::runtime_error(msg) {}
};

struct LastAction {
  int tick = 0;
  std::string action;
  bool moved = false;
  std::optional<std::string> effective_direction;
  std::optional<std::string> error;
};

struct SelfState {
  std::string team;
  std::string robot_id;
  Cell position;
  std::optional<std::string> position_reason;
  int hp = 0;
  bool alive = true;
  bool defending = false;
  bool protected_ = false;  // 出生保护（protected 为 C++ 关键字）
  int respawn_in = 0;
  std::optional<LastAction> last_action;
};

struct Entity {
  std::string type;  // robot / resource_normal / resource_high / obstacle
  Cell rel;          // 相对位移（由 9×9 位置串解析）
  Cell abs;          // 绝对坐标（rel + self.position）
  std::string id;
  std::string team;
  std::string robot_id;
  int hp = 0;
  bool defending = false;
  bool protected_ = false;
  int value = 0;
};

struct Score {
  double resource_value = 0.0;
  double effective_damage = 0.0;
  int mission_completed = 0;
  int invalid_actions = 0;
  std::vector<std::string> mission_completed_ids;
};

struct Mission {
  std::string id;
  std::string type;  // normal_resources / high_resources / resource_value /
                     // effective_damage / reach
  std::optional<int> target;
  std::optional<Cell> position;
};

struct Perception {
  int tick = 0;
  std::string session_id;
  int vision_radius = 4;
  SelfState self;
  std::vector<Entity> entities;
  Score score;
  std::string mission_text;
  std::vector<Mission> missions;
  int map_width = 0;
  int map_height = 0;
  std::string event_text;
};

// 解析视野实体的 9×9 相对位置串，返回相对位移 (dx, dy)。
// 字符串共 89 个字符（81 个格子字符 + 8 个换行），无末尾换行；
// 图中没有 '2'（物体与本车重叠）时返回 (0, 0)。
// 非法输入抛出 ProtocolError。
Cell parse_position_string(const std::string& text);

// 解析 /team_X/perception 的 data 内容。成功返回 true；
// 任何字段非法时返回 false 并填写 err（调用方丢弃该帧）。
bool parse_perception(const std::string& data, Perception& out, std::string& err);

// 构造 /team_X/action 的 data JSON 字符串；move/attack 必须带 direction。
// 消息超过 4096 字节或参数非法时抛出 ProtocolError。
std::string build_action(int tick, const std::string& session_id,
                         const std::string& action,
                         const std::optional<std::string>& direction);

// 构造 /team_X/registration 的 data JSON 字符串。
// 队名 UTF-8 编码 1–96 字节，不得包含换行或回车；非法时抛 ProtocolError。
std::string build_registration(const std::string& team, const std::string& name);

}  // namespace rmtc
