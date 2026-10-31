#include "rmtc_bot_cpp/protocol.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <utility>

using nlohmann::json;

namespace rmtc {

namespace {

void require_int(const json& v, const char* name, int low, int high, int& out) {
  if (!v.is_number_integer()) {
    throw ProtocolError(std::string(name) + " 须为整数");
  }
  const int value = v.get<int>();
  if (value < low || value > high) {
    throw ProtocolError(std::string(name) + " 超出范围 [" + std::to_string(low) +
                        ", " + std::to_string(high) + "]");
  }
  out = value;
}

void require_str(const json& v, const char* name, bool allow_empty,
                 std::string& out) {
  if (!v.is_string()) {
    throw ProtocolError(std::string(name) + " 须为字符串");
  }
  out = v.get<std::string>();
  if (!allow_empty && out.empty()) {
    throw ProtocolError(std::string(name) + " 不能为空");
  }
}

void require_bool(const json& v, const char* name, bool& out) {
  if (!v.is_boolean()) {
    throw ProtocolError(std::string(name) + " 须为布尔值");
  }
  out = v.get<bool>();
}

void require_number(const json& v, const char* name, double& out) {
  if (!v.is_number()) {
    throw ProtocolError(std::string(name) + " 须为数值");
  }
  out = v.get<double>();
}

void require_position(const json& v, const char* name, Cell& out) {
  if (!v.is_array() || v.size() != 2) {
    throw ProtocolError(std::string(name) + " 须为 [x, y]");
  }
  int x = 0;
  int y = 0;
  require_int(v[0], (std::string(name) + "[0]").c_str(), 0, INT32_MAX, x);
  require_int(v[1], (std::string(name) + "[1]").c_str(), 0, INT32_MAX, y);
  out = Cell{x, y};
}

LastAction parse_last_action(const json& value) {
  if (value.is_null()) {
    return LastAction{};
  }
  if (!value.is_object()) {
    throw ProtocolError("self.last_action 须为对象");
  }
  LastAction result;
  require_int(value.value("tick", json(0)), "self.last_action.tick", 0,
              INT32_MAX, result.tick);
  const auto& action = value.value("action", json(nullptr));
  if (!action.is_string() || !is_action(action.get<std::string>())) {
    throw ProtocolError("self.last_action.action 非法");
  }
  result.action = action.get<std::string>();
  // 协议只承诺成功移动给出 moved=true；攻击/防御/等待回执可能省略 moved，
  // 此时视为未移动（宽容解析，避免整帧被丢弃）。
  if (value.contains("moved") && !value["moved"].is_null()) {
    require_bool(value["moved"], "self.last_action.moved", result.moved);
  } else {
    result.moved = false;
  }
  if (value.contains("effective_direction") &&
      !value["effective_direction"].is_null()) {
    std::string dir;
    require_str(value["effective_direction"], "self.last_action.effective_direction",
                false, dir);
    if (!is_direction(dir)) {
      throw ProtocolError("self.last_action.effective_direction 非法");
    }
    result.effective_direction = dir;
  }
  if (value.contains("error") && !value["error"].is_null()) {
    std::string err;
    require_str(value["error"], "self.last_action.error", true, err);
    result.error = err;
  }
  return result;
}

SelfState parse_self(const json& value) {
  if (!value.is_object()) {
    throw ProtocolError("self 须为对象");
  }
  SelfState result;
  require_str(value.value("team", json(nullptr)), "self.team", false, result.team);
  require_str(value.value("robot_id", json(nullptr)), "self.robot_id", false,
              result.robot_id);
  require_position(value.value("position", json(nullptr)), "self.position",
                   result.position);
  require_int(value.value("hp", json(0)), "self.hp", 0, INT32_MAX, result.hp);
  require_bool(value.value("alive", json(false)), "self.alive", result.alive);
  require_bool(value.value("defending", json(false)), "self.defending",
               result.defending);
  require_int(value.value("respawn_in", json(0)), "self.respawn_in", 0, INT32_MAX,
              result.respawn_in);
  require_bool(value.value("protected", json(false)), "self.protected",
               result.protected_);
  if (value.contains("last_action")) {
    result.last_action = parse_last_action(value["last_action"]);
  }
  if (value.contains("position_reason") && !value["position_reason"].is_null()) {
    std::string reason;
    require_str(value["position_reason"], "self.position_reason", true, reason);
    result.position_reason = reason;
  }
  return result;
}

Entity parse_entity(const json& value) {
  if (!value.is_object()) {
    throw ProtocolError("visible_entities 元素须为对象");
  }
  const std::string type =
      value.value("type", json(nullptr)).is_string()
          ? value["type"].get<std::string>()
          : std::string();
  if (type.empty()) {
    throw ProtocolError("实体 type 须为非空字符串");
  }
  const auto& pos = value.value("position", json(nullptr));
  if (!pos.is_string()) {
    throw ProtocolError("实体 position 须为字符串");
  }
  Entity result;
  result.type = type;
  result.rel = parse_position_string(pos.get<std::string>());
  if (type == "robot") {
    require_str(value.value("id", json(nullptr)), "robot.id", false, result.id);
    require_str(value.value("team", json(nullptr)), "robot.team", false,
                result.team);
    require_str(value.value("robot_id", json(nullptr)), "robot.robot_id", false,
                result.robot_id);
    require_int(value.value("hp", json(0)), "robot.hp", 0, INT32_MAX, result.hp);
    require_bool(value.value("defending", json(false)), "robot.defending",
                 result.defending);
    require_bool(value.value("protected", json(false)), "robot.protected",
                 result.protected_);
  } else if (type == "resource_normal" || type == "resource_high") {
    require_str(value.value("id", json(nullptr)), "resource.id", false, result.id);
    require_int(value.value("value", json(0)), "resource.value", 1, INT32_MAX,
                result.value);
  } else if (type != "obstacle") {
    // 未知类型（如随机事件增益实体）宽容解析为可拾取目标，绝不因
    // 裁判新增实体类型而丢弃整帧感知。
    result.id = (value.contains("id") && value["id"].is_string())
                    ? value["id"].get<std::string>()
                    : std::string();
    if (value.value("value", json(nullptr)).is_number_integer()) {
      result.value = value["value"].get<int>();
      if (result.value < 0) {
        result.value = 0;
      }
    } else {
      result.value = 8;  // 无 value 字段的增益实体按中等价值资源对待
    }
  }
  return result;
}

Score parse_score(const json& value) {
  if (!value.is_object()) {
    throw ProtocolError("score 须为对象");
  }
  Score result;
  require_number(value.value("resource_value", json(0)), "score.resource_value",
                 result.resource_value);
  require_number(value.value("effective_damage", json(0)), "score.effective_damage",
                 result.effective_damage);
  require_int(value.value("mission_completed", json(0)), "score.mission_completed",
              0, INT32_MAX, result.mission_completed);
  require_int(value.value("invalid_actions", json(0)), "score.invalid_actions", 0,
              INT32_MAX, result.invalid_actions);
  const auto& ids = value.value("mission_completed_ids", json(nullptr));
  if (!ids.is_array()) {
    throw ProtocolError("score.mission_completed_ids 须为数组");
  }
  for (const auto& item : ids) {
    std::string id;
    require_str(item, "score.mission_completed_ids 元素", false, id);
    result.mission_completed_ids.push_back(id);
  }
  return result;
}

Mission parse_mission(const json& value) {
  if (!value.is_object()) {
    throw ProtocolError("missions 元素须为对象");
  }
  Mission result;
  require_str(value.value("id", json(nullptr)), "mission.id", false, result.id);
  const auto& type = value.value("type", json(nullptr));
  if (!type.is_string()) {
    throw ProtocolError("mission.type 须为字符串");
  }
  result.type = type.get<std::string>();
  // 宽容解析：裁判可能新增任务类型，未知类型照常保留（策略只使用 reach），
  // 绝不因新任务类型丢弃整帧感知。
  if (value.contains("target") && !value["target"].is_null()) {
    int target = 0;
    require_int(value["target"], "mission.target", 0, INT32_MAX, target);
    result.target = target;
  }
  if (value.contains("position") && !value["position"].is_null()) {
    Cell pos;
    require_position(value["position"], "mission.position", pos);
    result.position = pos;
  }
  return result;
}

}  // namespace

Cell dir_delta(const std::string& direction) {
  if (direction == "up") return Cell{0, -1};
  if (direction == "down") return Cell{0, 1};
  if (direction == "left") return Cell{-1, 0};
  if (direction == "right") return Cell{1, 0};
  throw ProtocolError("非法方向: " + direction);
}

std::optional<std::string> delta_dir(const Cell& delta) {
  if (delta == Cell{0, -1}) return std::string("up");
  if (delta == Cell{0, 1}) return std::string("down");
  if (delta == Cell{-1, 0}) return std::string("left");
  if (delta == Cell{1, 0}) return std::string("right");
  return std::nullopt;
}

std::optional<std::string> opposite(const std::string& direction) {
  if (direction == "up") return std::string("down");
  if (direction == "down") return std::string("up");
  if (direction == "left") return std::string("right");
  if (direction == "right") return std::string("left");
  return std::nullopt;
}

Cell parse_position_string(const std::string& text) {
  // 兼容 CRLF；协议约定无末尾换行，解析时容错。
  std::string normalized;
  normalized.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      continue;
    }
    normalized.push_back(text[i]);
  }
  std::vector<std::string_view> lines;
  size_t start = 0;
  for (size_t i = 0; i <= normalized.size(); ++i) {
    if (i == normalized.size() || normalized[i] == '\n') {
      lines.push_back(std::string_view(normalized).substr(start, i - start));
      start = i + 1;
    }
  }
  if (lines.size() != VIEW_SIZE) {
    throw ProtocolError("位置串须为 9 行");
  }
  std::optional<Cell> found;
  for (int row = 0; row < VIEW_SIZE; ++row) {
    const auto& line = lines[row];
    if (static_cast<int>(line.size()) != VIEW_SIZE) {
      throw ProtocolError("位置串每行须为 9 个字符");
    }
    for (int col = 0; col < VIEW_SIZE; ++col) {
      const char ch = line[col];
      if (ch != '0' && ch != '1' && ch != '2') {
        throw ProtocolError("位置串只允许 0/1/2");
      }
      if (ch == '1' && (row != VIEW_CENTER || col != VIEW_CENTER)) {
        throw ProtocolError("位置串中 1 必须固定在第 5 行第 5 列");
      }
      if (ch == '2') {
        if (found.has_value()) {
          throw ProtocolError("位置串中只允许一个 2");
        }
        found = Cell{col - VIEW_CENTER, row - VIEW_CENTER};
      }
    }
  }
  return found.value_or(Cell{0, 0});
}

bool parse_perception(const std::string& data, Perception& out,
                      std::string& err) {
  try {
    json obj = json::parse(data);
    if (!obj.is_object()) {
      err = "感知须为 JSON 对象";
      return false;
    }
    Perception p;
    require_int(obj.value("tick", json(0)), "tick", 0, INT32_MAX, p.tick);
    require_str(obj.value("session_id", json(nullptr)), "session_id", false,
                p.session_id);
    require_int(obj.value("vision_radius", json(4)), "vision_radius", 0, 4,
                p.vision_radius);
    p.self = parse_self(obj.value("self", json(nullptr)));
    const auto& map_info = obj.value("map", json(nullptr));
    if (!map_info.is_object()) {
      throw ProtocolError("map 须为对象");
    }
    require_int(map_info.value("width", json(0)), "map.width", 1, INT32_MAX,
                p.map_width);
    require_int(map_info.value("height", json(0)), "map.height", 1, INT32_MAX,
                p.map_height);
    const auto& entities = obj.value("visible_entities", json(nullptr));
    if (!entities.is_array()) {
      throw ProtocolError("visible_entities 须为数组");
    }
    for (const auto& raw : entities) {
      Entity entity = parse_entity(raw);
      entity.abs = Cell{p.self.position.x + entity.rel.x,
                        p.self.position.y + entity.rel.y};
      p.entities.push_back(std::move(entity));
    }
    const auto& missions = obj.value("missions", json(nullptr));
    if (!missions.is_array()) {
      throw ProtocolError("missions 须为数组");
    }
    for (const auto& raw : missions) {
      p.missions.push_back(parse_mission(raw));
    }
    p.score = parse_score(obj.value("score", json(nullptr)));
    require_str(obj.value("mission_text", json("")), "mission_text", true,
                p.mission_text);
    require_str(obj.value("event_text", json("")), "event_text", true,
                p.event_text);
    out = std::move(p);
    return true;
  } catch (const ProtocolError& e) {
    err = e.what();
    return false;
  } catch (const json::exception& e) {
    err = std::string("JSON 解析失败: ") + e.what();
    return false;
  } catch (const std::exception& e) {
    err = std::string("感知解析异常: ") + e.what();
    return false;
  }
}

std::string build_action(int tick, const std::string& session_id,
                         const std::string& action,
                         const std::optional<std::string>& direction) {
  if (!is_action(action)) {
    throw ProtocolError("非法动作: " + action);
  }
  if (direction.has_value() && !is_direction(*direction)) {
    throw ProtocolError("非法方向: " + *direction);
  }
  if ((action == "move" || action == "attack") && !direction.has_value()) {
    throw ProtocolError(action + " 必须携带 direction");
  }
  json payload;
  payload["tick"] = tick;
  payload["session_id"] = session_id;
  payload["action"] = action;
  if (direction.has_value()) {
    payload["direction"] = *direction;
  }
  const std::string data = payload.dump();
  if (data.size() > MAX_ACTION_BYTES) {
    throw ProtocolError("动作消息超过 4096 字节");
  }
  return data;
}

std::string build_registration(const std::string& team, const std::string& name) {
  if (team.empty() || team.size() > 16) {
    throw ProtocolError("席位须为 1–16 字节字符串");
  }
  size_t name_bytes = 0;
  for (unsigned char ch : name) {
    if (ch == '\n' || ch == '\r') {
      throw ProtocolError("队名不得包含换行或回车");
    }
    ++name_bytes;
  }
  if (name_bytes < 1 || name_bytes > 96) {
    throw ProtocolError("队名 UTF-8 编码长度须为 1–96 字节");
  }
  json payload;
  payload["team"] = team;
  payload["name"] = name;
  const std::string data = payload.dump();
  if (data.size() > MAX_ACTION_BYTES) {
    throw ProtocolError("注册消息超过 4096 字节");
  }
  return data;
}

}  // namespace rmtc
