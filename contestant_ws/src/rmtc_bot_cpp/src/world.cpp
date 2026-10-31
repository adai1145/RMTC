#include "rmtc_bot_cpp/world.hpp"

#include <algorithm>

namespace rmtc {

void WorldModel::reset(const std::string& new_session_id) {
  // 名录在首帧前由 transient_local 送达；场次切换的 reset 不得清掉它，
  // 否则无任务/无敌人的第二场会因失去多席位信号而退回第一阶段。
  std::unordered_set<std::string> teams = std::move(registry_teams);
  *this = WorldModel{};
  registry_teams = std::move(teams);
  session_id = new_session_id;
  if (registry_teams.size() > 1 && stage == 1) {
    stage = 2;
  }
}

void WorldModel::set_registry(const std::vector<std::string>& teams) {
  registry_teams.clear();
  for (const auto& team : teams) {
    registry_teams.insert(team);
  }
  if (registry_teams.size() > 1 && stage == 1) {
    stage = 2;
  }
}

void WorldModel::note_command(const std::string& action,
                              const std::optional<std::string>& direction,
                              int tick) {
  last_command = Command{tick, action, direction};
}

void WorldModel::note_dropped_frame() { ++frames_dropped; }

WorldEvents WorldModel::update(const Perception& perception) {
  WorldEvents events;
  // 旧状态必须先于 reset 捕获：reset 会把 tick/alive/invalid_actions 清零，
  // 若之后再读就会把新场次首帧误判为“重发/复活/无效动作归零”。
  const bool session_changed = perception.session_id != session_id;
  int prev_tick = tick;
  bool prev_alive = self_state.alive;
  int prev_invalid = invalid_actions_seen;
  if (session_changed) {
    reset(perception.session_id);
    events.new_session = true;
    prev_tick = -1;      // 跨场次无“上一 Tick”可比
    prev_alive = false;
    prev_invalid = 0;
  }
  tick = perception.tick;
  ++frames_received;

  map_width = perception.map_width;
  map_height = perception.map_height;
  vision_radius = perception.vision_radius;
  self_state = perception.self;
  if (self_state.alive) {
    visited.insert(self_state.position);
  }

  learn_inversion(self_state.last_action);

  const auto& entities = perception.entities;
  last_entities = entities;
  if (self_state.alive) {
    merge_vision(self_state.position, entities);
  }

  score = perception.score;
  missions = perception.missions;
  mission_text = perception.mission_text;
  mission_completed_ids.clear();
  for (const auto& id : perception.score.mission_completed_ids) {
    mission_completed_ids.insert(id);
  }
  if (perception.score.invalid_actions > prev_invalid) {
    events.invalid_actions_increased = true;
  }
  invalid_actions_seen = perception.score.invalid_actions;

  merge_announcements(perception.event_text);
  if (!new_announcements.empty()) {
    events.new_announcements = true;
  }

  if (stage == 1) {
    bool has_robot = false;
    for (const auto& e : entities) {
      if (e.type == "robot") {
        has_robot = true;
        break;
      }
    }
    if (!missions.empty() || has_robot || registry_teams.size() > 1) {
      stage = 2;
      events.stage2_detected = true;
    }
  }

  if (!prev_alive && self_state.alive) {
    events.respawned = true;
  }
  if (prev_tick == tick) {
    events.redelivery = true;
  }
  return events;
}

std::vector<Cell> WorldModel::cells_in_vision(const Cell& position) const {
  std::vector<Cell> cells;
  cells.reserve(static_cast<size_t>((vision_radius * 2 + 1) *
                                    (vision_radius * 2 + 1)));
  for (int dx = -vision_radius; dx <= vision_radius; ++dx) {
    for (int dy = -vision_radius; dy <= vision_radius; ++dy) {
      const Cell cell{position.x + dx, position.y + dy};
      if (in_bounds(cell, map_width, map_height)) {
        cells.push_back(cell);
      }
    }
  }
  return cells;
}

void WorldModel::merge_vision(const Cell& position,
                              const std::vector<Entity>& entities) {
  // 视野内障碍/资源以报告为准增删；敌人更新并维护过期。
  std::unordered_set<Cell> reported_obstacles;
  std::unordered_map<Cell, ResourceInfo> reported_resources;
  std::unordered_set<std::string> reported_enemies;

  for (const auto& entity : entities) {
    const Cell cell = entity.abs;
    if (entity.type == "obstacle") {
      reported_obstacles.insert(cell);
    } else if (entity.type != "robot") {
      // 所有非机器人实体（含随机事件增益等未知类型）都视为可拾取目标
      reported_resources[cell] = ResourceInfo{entity.id, entity.value, tick};
    } else if (entity.type == "robot") {
      reported_enemies.insert(entity.robot_id);
      enemies[entity.robot_id] = EnemyInfo{entity.robot_id,
                                           entity.team,
                                           cell,
                                           entity.hp,
                                           entity.defending,
                                           entity.protected_,
                                           tick};
    }
  }

  const std::vector<Cell> visible = cells_in_vision(position);
  std::unordered_set<Cell> visible_set(visible.begin(), visible.end());

  for (auto it = obstacles.begin(); it != obstacles.end();) {
    if (visible_set.count(*it) > 0 && reported_obstacles.count(*it) == 0) {
      it = obstacles.erase(it);  // 临时障碍消失或此前误记
    } else {
      ++it;
    }
  }
  obstacles.insert(reported_obstacles.begin(), reported_obstacles.end());

  for (auto it = resources.begin(); it != resources.end();) {
    if (visible_set.count(it->first) > 0 &&
        reported_resources.count(it->first) == 0) {
      it = resources.erase(it);  // 已收集
    } else {
      ++it;
    }
  }
  resources.insert(reported_resources.begin(), reported_resources.end());

  for (auto it = enemies.begin(); it != enemies.end();) {
    const EnemyInfo& enemy = it->second;
    const bool gone_from_vision =
        reported_enemies.count(it->first) == 0 &&
        visible_set.count(enemy.position) > 0;
    if (tick - enemy.last_seen > ENEMY_STALE_TICKS || gone_from_vision) {
      it = enemies.erase(it);
    } else {
      ++it;
    }
  }
}

bool WorldModel::merge_announcement(const std::string& line) {
  std::string stripped = line;
  const auto is_space = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!stripped.empty() && is_space(stripped.front())) {
    stripped.erase(stripped.begin());
  }
  while (!stripped.empty() && is_space(stripped.back())) {
    stripped.pop_back();
  }
  if (stripped.empty() || announcement_set.count(stripped) > 0) {
    return false;
  }
  announcement_set.insert(stripped);
  announcements.push_back(stripped);
  if (static_cast<int>(announcements.size()) > ANNOUNCE_WINDOW) {
    announcements.erase(announcements.begin(),
                        announcements.end() - ANNOUNCE_WINDOW);
  }
  return true;
}

void WorldModel::merge_announcements(const std::string& event_text) {
  new_announcements.clear();
  size_t start = 0;
  while (start <= event_text.size()) {
    const size_t pos = event_text.find('\n', start);
    const std::string line =
        event_text.substr(start, pos == std::string::npos
                                     ? std::string::npos
                                     : pos - start);
    if (merge_announcement(line)) {
      new_announcements.push_back(line);
    }
    if (pos == std::string::npos) {
      break;
    }
    start = pos + 1;
  }
}

void WorldModel::learn_inversion(const std::optional<LastAction>& receipt) {
  // 协议约定 effective_direction 已经是实际执行方向，这里只比较
  // “命令方向 vs 实际方向”是否相反，绝不对回执二次反转。
  last_receipt = receipt;
  if (!receipt.has_value() || !last_command.has_value() ||
      last_command->action != "move" || receipt->tick != last_command->tick) {
    return;
  }
  const auto& commanded = last_command->direction;
  if (!commanded.has_value()) {
    return;
  }
  if (receipt->moved) {
    const auto& effective = receipt->effective_direction;
    if (!effective.has_value()) {
      return;
    }
    const auto opp = opposite(*commanded);
    if (opp.has_value() && *effective == *opp) {
      inverted = true;
    } else if (*effective == *commanded) {
      inverted = false;
    }
    return;
  }
  // 幻术死锁探测：moved=false 且被阻挡/出界，但按本地地图知识该落点
  // 明明可通行——说明实际执行方向被幻术反转（真方向撞墙/出界），推断反转。
  // 误判会在下一次 moved=true 回执里自动纠正。
  if (receipt->error.has_value() &&
      (receipt->error->find("BLOCKED") != std::string::npos ||
       receipt->error->find("OOB") != std::string::npos)) {
    try {
      const Cell delta = dir_delta(*commanded);
      const Cell intended{position().x + delta.x, position().y + delta.y};
      if (in_bounds(intended, map_width, map_height) &&
          obstacles.count(intended) == 0) {
        bool robot_here = false;
        for (const auto& e : last_entities) {
          if (e.type == "robot" && e.abs == intended) {
            robot_here = true;
            break;
          }
        }
        if (!robot_here) {
          inverted = true;
        }
      }
    } catch (const ProtocolError&) {
    }
  }
}

Cell WorldModel::position() const { return self_state.position; }

bool WorldModel::alive() const { return self_state.alive; }

int WorldModel::hp() const { return self_state.hp; }

bool WorldModel::protected_now() const { return self_state.protected_; }

bool WorldModel::defending() const { return self_state.defending; }

int WorldModel::respawn_in() const { return self_state.respawn_in; }

std::vector<EnemyInfo> WorldModel::fresh_enemies(int max_age) const {
  std::vector<EnemyInfo> result;
  for (const auto& [robot_id, enemy] : enemies) {
    (void)robot_id;
    if (tick - enemy.last_seen <= max_age) {
      result.push_back(enemy);
    }
  }
  return result;
}

std::unordered_set<std::string> WorldModel::visible_enemy_robot_ids() const {
  std::unordered_set<std::string> result;
  for (const auto& e : last_entities) {
    if (e.type == "robot") {
      result.insert(e.robot_id);
    }
  }
  return result;
}

std::vector<std::pair<Cell, ResourceInfo>> WorldModel::resource_cells(
    int max_age) const {
  std::vector<std::pair<Cell, ResourceInfo>> result;
  for (const auto& [cell, info] : resources) {
    if (max_age < 0 || tick - info.last_seen <= max_age) {
      result.emplace_back(cell, info);
    }
  }
  return result;
}

const Mission* WorldModel::reach_mission() const {
  for (const auto& mission : missions) {
    if (mission.type == "reach" &&
        mission_completed_ids.count(mission.id) == 0) {
      return &mission;
    }
  }
  return nullptr;
}

std::pair<int, int> WorldModel::mission_counts() const {
  const int value = static_cast<int>(score.resource_value);
  const int high = value / 20;
  const int normal = (value - high * 20) / 10;
  return {normal, high};
}

}  // namespace rmtc
