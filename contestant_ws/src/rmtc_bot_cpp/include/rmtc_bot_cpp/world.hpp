#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rmtc_bot_cpp/protocol.hpp"
#include "rmtc_bot_cpp/types.hpp"

namespace rmtc {

constexpr int ENEMY_STALE_TICKS = 20;  // 敌人最后目击超过该 Tick 数后不再用于追击
constexpr int ANNOUNCE_WINDOW = 24;    // 保留最近公告条数（提示词窗口；额度保护：
                                       // 窗口过大会让已结束事件的旧公告反复占 token）

struct ResourceInfo {
  std::string id;
  int value = 0;
  int last_seen = 0;
};

struct EnemyInfo {
  std::string robot_id;
  std::string team;
  Cell position;
  int hp = 0;
  bool defending = false;
  bool protected_ = false;
  int last_seen = 0;
};

struct Command {
  int tick = 0;
  std::string action;
  std::optional<std::string> direction;
};

struct WorldEvents {
  bool new_session = false;
  bool stage2_detected = false;
  bool invalid_actions_increased = false;
  bool new_announcements = false;
  bool respawned = false;
  bool redelivery = false;
};

// 单场次状态记忆。session_id 变化时整体重置。
//
// 原则：
// - self.position 每帧以感知绝对坐标为准，绝不自行积分位移；
// - 障碍/资源按视野刷新：视野内消失即移除（兼容临时障碍与已收集资源）；
// - 敌人离开视野后保留最后位置并随时间过期，只用于追击参考；
// - 幻术不解析公告文本，完全通过动作回执的 effective_direction 反馈学习；
// - 随机事件公告仅原样存储转发给 LLM，不在本模块做任何文本解析。
class WorldModel {
 public:
  std::string session_id;
  int tick = -1;
  int map_width = 0;
  int map_height = 0;
  int vision_radius = 4;
  SelfState self_state;
  std::unordered_set<Cell> obstacles;
  std::unordered_map<Cell, ResourceInfo> resources;
  std::unordered_map<std::string, EnemyInfo> enemies;
  Score score;
  std::vector<Mission> missions;
  std::string mission_text;
  std::unordered_set<std::string> mission_completed_ids;
  std::vector<std::string> announcements;
  std::unordered_set<std::string> announcement_set;
  int stage = 1;
  std::unordered_set<std::string> registry_teams;
  bool inverted = false;
  std::optional<Command> last_command;
  std::optional<LastAction> last_receipt;
  std::vector<Entity> last_entities;
  std::unordered_set<Cell> visited;
  int frames_received = 0;
  int frames_dropped = 0;
  int invalid_actions_seen = 0;
  std::vector<std::string> new_announcements;

  void reset(const std::string& new_session_id);
  void set_registry(const std::vector<std::string>& teams);
  void note_command(const std::string& action,
                    const std::optional<std::string>& direction, int tick);
  void note_dropped_frame();

  // 应用一帧合法感知，返回事件标志供上层决策。
  WorldEvents update(const Perception& perception);

  // 按原文去重并入一条公告；返回是否为新公告。不做任何文本解析。
  bool merge_announcement(const std::string& line);

  Cell position() const;
  bool alive() const;
  int hp() const;
  bool protected_now() const;
  bool defending() const;
  int respawn_in() const;

  // 最近 max_age Tick 内目击过的敌人（用于战斗与躲避）。
  std::vector<EnemyInfo> fresh_enemies(int max_age = ENEMY_STALE_TICKS) const;

  // 本帧视野内敌人的 robot_id。
  std::unordered_set<std::string> visible_enemy_robot_ids() const;

  // 记忆中的资源位置；max_age < 0 表示不限。
  std::vector<std::pair<Cell, ResourceInfo>> resource_cells(int max_age = -1) const;

  // 未完成的 reach 任务；已完成或不存在返回 nullptr。
  const Mission* reach_mission() const;

  // 本队已收集的普通/高价值资源数量（按资源价值推断，仅用于日志展示）。
  std::pair<int, int> mission_counts() const;

 private:
  std::vector<Cell> cells_in_vision(const Cell& position) const;
  void merge_vision(const Cell& position, const std::vector<Entity>& entities);
  void merge_announcements(const std::string& event_text);
  void learn_inversion(const std::optional<LastAction>& receipt);
};

}  // namespace rmtc
