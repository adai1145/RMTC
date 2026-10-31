#include "rmtc_bot_cpp/strategy.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "rmtc_bot_cpp/planner.hpp"
#include "rmtc_bot_cpp/protocol.hpp"

using nlohmann::json;

namespace rmtc {

namespace {

constexpr std::string_view kDirections[] = {"up", "down", "left", "right"};
constexpr int kDefenderKillHp = 10;  // 防御减伤后每炮 10 伤
constexpr int kKillHp = 20;          // 单发 20 伤：HP≤20 一炮击杀
constexpr int kPeerlessDamage = 50;         // 天下无双：非防御目标单发 50 伤
constexpr int kPeerlessDefenderDamage = 10; // 天下无双：防御目标单发 10 伤
constexpr int kPeerlessRange = 2;           // 天下无双：横纵 ±2 的 5×5 攻击范围
constexpr int kResidencyTicks = 10;         // 天下无双：连续驻留达标 Tick 数

int danger_penalty(const std::string& decision_type) {
  if (decision_type == "collect") return 8;
  if (decision_type == "combat") return 2;
  return 4;  // balanced
}

std::vector<Entity> visible_robots(const WorldModel& world) {
  std::vector<Entity> visible;
  for (const auto& e : world.last_entities) {
    if (e.type == "robot") {
      visible.push_back(e);
    }
  }
  return visible;
}

}  // namespace

Decision final_guard(const WorldModel& world, const std::string& action,
                     const std::optional<std::string>& direction) {
  if (!world.alive()) {
    return Decision{"wait", std::nullopt};
  }
  if (action == "attack" && (world.protected_now() || world.stage != 2)) {
    return Decision{"wait", std::nullopt};
  }
  if (action == "defend" && world.stage == 1) {
    return Decision{"wait", std::nullopt};
  }
  if (action != "move" && action != "attack") {
    return Decision{action, std::nullopt};  // wait/defend 不得携带 direction 字段
  }
  if ((action == "move" || action == "attack") &&
      (!direction.has_value() || !is_direction(*direction))) {
    return Decision{"wait", std::nullopt};
  }
  return Decision{action, direction};
}

Strategy::Strategy(std::string decision_type, LogFn log)
    : decision_type(std::move(decision_type)), log_(std::move(log)) {
  if (this->decision_type != "collect" && this->decision_type != "combat" &&
      this->decision_type != "balanced") {
    this->decision_type = "balanced";
  }
}

void Strategy::set_params(const StrategyParams& params) { params_ = params; }

// ================================================================ 主入口
Decision Strategy::decide(WorldModel& world,
                          const std::optional<Advice>& advice) {
  auto hard = hard_guard(world);
  if (hard.has_value()) {
    return *hard;
  }

  // 事件计划按场次隔离：绝对 Tick 在新场次从 0 重新计数，不清除会让旧窗口
  // 永久生效或提前失效。
  if (plan_session_ != world.session_id) {
    plan_session_ = world.session_id;
    plan_invert_ = false;
    plan_invert_until_ = -1;
  }
  if (event_session_ != world.session_id) {
    event_session_ = world.session_id;
    reflection_from_ = 0;
    reflection_until_ = -1;
    announcement_llm_pending_ = false;
  }
  if (peerless_session_ != world.session_id) {
    peerless_session_ = world.session_id;
    fortress_from_ = 0;
    fortress_until_ = -1;
    residency_streak_ = 0;
    peerless_granted_ = false;
  }

  prune_goal_priority(world);

  // goals 优先级、事件计划与反伤窗口先于任何决策更新：即使计划动作本次
  // 不被采纳，窗口信息也即刻生效，供本地规则使用。
  auto fresh = fresh_advice(world, advice);
  if (fresh.has_value()) {
    update_goal_priority(world, *fresh);
    update_plan_state(world, *fresh);
    update_event_state(world, *fresh);
    update_fortress_state(world, *fresh);
  }
  // 驻留连击与增益授予在每帧决策前结算：达标后本帧即可按增益行动
  // （规则：达标在该 Tick 战斗后授予，之后须主动提交 ATTACK）。
  update_residency(world);

  if (world.stage == 2) {
    // 事件计划先于即时战术：公告给出的撤离/等待/防御窗口本地无法自行推断。
    // 计划自身的生存让位规则见 apply_event_plan。
    if (fresh.has_value()) {
      auto plan = apply_event_plan(world, *fresh);
      if (plan.has_value()) {
        return *plan;
      }
    }
    auto tactical = tactical_decide(world);
    if (tactical.has_value()) {
      return *tactical;
    }
  }

  return rule_decide(world);
}

// ================================================================ 硬约束
std::optional<Decision> Strategy::hard_guard(const WorldModel& world) {
  if (world.session_id.empty()) {
    return Decision{"wait", std::nullopt};
  }
  if (!world.alive()) {
    last_decision_reason = "阵亡等待复活";
    return Decision{"wait", std::nullopt};
  }
  return std::nullopt;
}

// ================================================================ 即时战术
std::optional<Decision> Strategy::tactical_decide(WorldModel& world) {
  const std::vector<Entity> visible = visible_robots(world);
  const auto threats = threat_cells(world, visible);
  const bool in_threat = threats.count(world.position()) > 0;

  if (world.protected_now()) {
    // 保护按 Tick 自然到期，原地等待不会延长；保护期内 ATTACK 会被裁判拒绝，
    // 窗口内唯一有价值的动作是移动，交给目标竞争/追击决定脱离还是贴脸。
    return std::nullopt;  // 按常规目标推进（不得攻击）
  }

  auto attack = choose_attack(world);
  if (attack.has_value()) {
    // 同 Tick 互杀不计击杀奖励：残血身处敌方火力线时，若防御减伤后
    // 可存活，先保命（DEFEND 是残血唯一合法的等等看）；防御也扛不住
    // 一击（必死）才换命止损——攻击至少计入有效伤害。
    constexpr int kPerHit = 20;
    if (in_threat && world.hp() <= kPerHit) {
      if (world.hp() > kPerHit / 2) {
        last_decision_reason = "残血身处火力线，防御保命优先于换命";
        return Decision{"defend", std::nullopt};
      }
      last_decision_reason = "防御亦难保命，换命止损";
    }
    last_decision_reason = peerless_active(world)
                               ? "攻击（天下无双 5×5 范围）"
                               : "攻击 " + *attack + "（敌人在直线射程内）";
    return Decision{"attack", attack};
  }

  // 洁身自好窗口：身处火力线时防御反伤，比逃离更划算。
  if (in_threat && reflection_active(world)) {
    last_decision_reason = "洁身自好窗口，防御反伤";
    return Decision{"defend", std::nullopt};
  }

  if (in_threat) {
    auto escape = escape_move(world, visible);
    if (escape.has_value()) {
      last_decision_reason = "脱离敌方火力线";
      return Decision{"move", escape};
    }
    last_decision_reason = "无法脱离，防御减伤";
    return Decision{"defend", std::nullopt};
  }
  return std::nullopt;
}

// ================================================================ LLM 建议
std::optional<Advice> Strategy::fresh_advice(
    const WorldModel& world, const std::optional<Advice>& advice) const {
  if (!advice.has_value()) {
    return std::nullopt;
  }
  if (advice->session_id != world.session_id) {
    return std::nullopt;
  }
  // goals 与 event_plan 各自按绝对 Tick 过期（goal_priority_until_ /
  // plan_until_tick），不因建议年龄整条丢弃：事件计划可能比建议本身更长寿。
  return advice;
}

int Strategy::plan_until_tick(const Advice& advice) const {
  if (!advice.event_plan.has_value()) {
    return -1;
  }
  // 本地封顶：LLM 自报的 until_tick 只能缩短、不能突破生命周期上限。
  return std::min(advice.event_plan->until_tick,
                  advice.tick + LLM_PLAN_MAX_LIFETIME);
}

void Strategy::update_plan_state(const WorldModel& world,
                                 const Advice& advice) {
  if (plan_session_ != world.session_id) {
    plan_session_ = world.session_id;
    plan_invert_ = false;
    plan_invert_until_ = -1;
  }
  if (!advice.event_plan.has_value()) {
    return;  // goals-only 建议不撤销仍在有效期内的幻术补偿
  }
  const EventPlan& plan = *advice.event_plan;
  const int until = plan_until_tick(advice);
  if (plan.mode == "normal" || until < world.tick || !plan.invert_controls) {
    plan_invert_ = false;
    plan_invert_until_ = -1;
    return;
  }
  plan_invert_ = true;
  plan_invert_until_ = until;
}

void Strategy::update_event_state(const WorldModel& world,
                                  const Advice& advice) {
  if (event_session_ != world.session_id) {
    event_session_ = world.session_id;
    reflection_from_ = 0;
    reflection_until_ = -1;
    announcement_llm_pending_ = false;
  }
  if (advice.reflection_until <= 0) {
    return;  // 建议未给出窗口：保留仍在有效期内的旧窗口
  }
  if (advice.reflection_until < world.tick) {
    return;  // 已过期，不复活
  }
  // 预告阶段即可填写 from：窗口只在 [from, until] 内生效，不会提前误判。
  const int from =
      advice.reflection_from > 0 ? advice.reflection_from : advice.tick;
  // 本地封顶与事件计划一致：单条建议不能把窗口拉到生命周期上限之外。
  const int until =
      std::min(advice.reflection_until, advice.tick + LLM_PLAN_MAX_LIFETIME);
  if (until < from || until <= reflection_until_) {
    return;  // 起止颠倒，或不比已记录窗口更长：不缩短、不覆盖
  }
  reflection_from_ = from;
  reflection_until_ = until;
}

bool Strategy::reflection_active(const WorldModel& world) const {
  return event_session_ == world.session_id && world.tick >= reflection_from_ &&
         world.tick <= reflection_until_;
}

// ================================================================ 天下无双
void Strategy::update_fortress_state(const WorldModel& world,
                                     const Advice& advice) {
  if (peerless_session_ != world.session_id) {
    peerless_session_ = world.session_id;
    fortress_from_ = 0;
    fortress_until_ = -1;
    residency_streak_ = 0;
    peerless_granted_ = false;
  }
  if (advice.fortress_until <= 0) {
    return;  // 建议未给出窗口：保留仍在有效期内的旧窗口
  }
  if (advice.fortress_until < world.tick) {
    return;  // 已过期，不复活
  }
  const int from =
      advice.fortress_from > 0 ? advice.fortress_from : advice.tick;
  // 本地封顶与事件计划一致：单条建议不能把窗口拉到生命周期上限之外。
  const int until =
      std::min(advice.fortress_until, advice.tick + LLM_PLAN_MAX_LIFETIME);
  if (until < from || until <= fortress_until_) {
    return;  // 起止颠倒，或不比已记录窗口更长：不缩短、不覆盖
  }
  fortress_from_ = from;
  fortress_until_ = until;
}

bool Strategy::fortress_active(const WorldModel& world) const {
  return peerless_session_ == world.session_id && fortress_until_ > 0 &&
         world.tick >= fortress_from_ && world.tick <= fortress_until_;
}

bool Strategy::peerless_active(const WorldModel& world) const {
  return fortress_active(world) && peerless_granted_;
}

bool Strategy::in_fortress(const WorldModel& world, const Cell& cell) const {
  if (world.map_width <= 0 || world.map_height <= 0) {
    return false;
  }
  // 堡垒为地图中心 3×3（以整数除法中心为基准）。
  const int cx = world.map_width / 2;
  const int cy = world.map_height / 2;
  return std::abs(cell.x - cx) <= 1 && std::abs(cell.y - cy) <= 1;
}

void Strategy::update_residency(WorldModel& world) {
  // 堡垒结束统一失效；窗口未激活或已结束时清空进度与增益。
  if (!fortress_active(world)) {
    residency_streak_ = 0;
    peerless_granted_ = false;
    return;
  }
  if (peerless_granted_) {
    return;  // 已授予：死亡复活也保留，直至堡垒结束
  }
  // 结算上一 Tick：战斗后存活、移动成功且移动后位于堡垒内才累计驻留；
  // 离开或死亡由下一次结算自然清空连击。回执必须对应当前帧的前一 Tick，
  // 陈旧回执不重复计费。
  const auto& receipt = world.last_receipt;
  const bool counted =
      world.stage == 2 && world.alive() &&
      in_fortress(world, world.position()) && receipt.has_value() &&
      receipt->tick == world.tick - 1 && receipt->action == "move" &&
      receipt->moved && receipt->tick >= fortress_from_ &&
      receipt->tick <= fortress_until_;
  residency_streak_ = counted ? residency_streak_ + 1 : 0;
  if (residency_streak_ >= kResidencyTicks) {
    peerless_granted_ = true;
    residency_streak_ = 0;
  }
}

std::optional<std::string> Strategy::residency_move(WorldModel& world) {
  // 堡垒内小幅移动以累计驻留：移动后必须仍在堡垒内，否则该 Tick 不计数。
  const Cell pos = world.position();
  for (const auto& direction : kDirections) {
    const std::string dir(direction);
    const Cell delta = dir_delta(dir);
    const Cell cell{pos.x + delta.x, pos.y + delta.y};
    if (!in_bounds(cell, world.map_width, world.map_height)) {
      continue;
    }
    if (!in_fortress(world, cell)) {
      continue;
    }
    if (world.obstacles.count(cell) > 0 || occupied_by_robot(world, cell)) {
      continue;
    }
    return apply_inversion(world, dir);
  }
  return std::nullopt;
}

std::optional<Decision> Strategy::apply_event_plan(WorldModel& world,
                                                   const Advice& advice) {
  if (!advice.event_plan.has_value()) {
    return std::nullopt;
  }
  const EventPlan& plan = *advice.event_plan;
  if (plan.mode == "normal") {
    return std::nullopt;
  }
  const int until = plan_until_tick(advice);
  if (world.tick > until) {
    return std::nullopt;  // 绝对 Tick 过期：回退本地启发式
  }

  if (plan.mode == "wait") {
    // 计划要求等待，但敌方火力线已覆盖本车时先执行本地生存动作，
    // 否则“原地等”会白挨一炮。
    const auto threats = threat_cells(world, visible_robots(world));
    if (threats.count(world.position()) > 0) {
      return std::nullopt;
    }
    last_decision_reason =
        "执行 LLM 事件计划: 等待至 Tick " + std::to_string(until);
    return Decision{"wait", std::nullopt};
  }
  if (plan.mode == "defend") {
    last_decision_reason =
        "执行 LLM 事件计划: 防御至 Tick " + std::to_string(until);
    return Decision{"defend", std::nullopt};
  }
  if (plan.mode == "move_to" && plan.target.has_value()) {
    const Cell target = *plan.target;
    if (!in_bounds(target, world.map_width, world.map_height) ||
        target == world.position()) {
      return std::nullopt;  // LLM 幻觉坐标：本地丢弃，回退常规规则
    }
    auto move = move_toward(world, target, enemy_danger(world));
    if (!move.has_value()) {
      return std::nullopt;
    }
    last_decision_reason = "执行 LLM 事件计划: 前往 [" +
                           std::to_string(target.x) + ", " +
                           std::to_string(target.y) + "] 至 Tick " +
                           std::to_string(until);
    return Decision{"move", move};
  }
  return std::nullopt;
}

bool Strategy::last_move_blocked(const WorldModel& world) const {
  const auto& receipt = world.last_receipt;
  return receipt.has_value() && !receipt->moved && receipt->action == "move";
}

// ================================================================ 高层目标
void Strategy::prune_goal_priority(const WorldModel& world) {
  if (goal_priority_session_ != world.session_id) {
    goal_priority_session_ = world.session_id;
    goal_priority_.clear();
    goal_priority_until_ = -1;
    return;
  }
  if (world.tick > goal_priority_until_) {
    goal_priority_.clear();
    goal_priority_until_ = -1;
  }
}

void Strategy::update_goal_priority(const WorldModel& world,
                                    const Advice& advice) {
  if (goal_priority_session_ != world.session_id) {
    goal_priority_session_ = world.session_id;
  }
  // 新建议即使没有 goals 也覆盖旧优先级，避免旧目标在场上“阴魂不散”。
  goal_priority_ = advice.goals;
  goal_priority_until_ =
      goal_priority_.empty() ? -1 : advice.tick + ADVICE_MAX_AGE;
}

int Strategy::goal_priority_bonus(const WorldModel& world,
                                  const std::string& type) const {
  if (goal_priority_.empty() || goal_priority_session_ != world.session_id ||
      world.tick > goal_priority_until_) {
    return 0;
  }
  for (size_t i = 0; i < goal_priority_.size(); ++i) {
    if (goal_priority_[i] == type) {
      const int rank = static_cast<int>(i) + 1;
      // 第一优先级拿到完整 llm_goal_weight，之后按排名递减；
      // 这是轻量偏置，不会压过本地战斗/威胁规则；配置为 0 即关闭。
      return std::max(0, params_.llm_goal_weight / rank);
    }
  }
  return 0;
}

// ================================================================ 本地规则
Decision Strategy::rule_decide(WorldModel& world) {
  if (world.stage == 1) {
    return stage1_decide(world);
  }
  return proceed_toward_goals(world);
}

Decision Strategy::stage1_decide(WorldModel& world) {
  auto move = goal_move(world);
  if (move.has_value()) {
    return Decision{"move", move};
  }
  move = explore_move(world);
  if (move.has_value()) {
    last_decision_reason = "探索未知区域";
    return Decision{"move", move};
  }
  last_decision_reason = "无可采集目标，等待";
  return Decision{"wait", std::nullopt};
}

Decision Strategy::proceed_toward_goals(WorldModel& world) {
  // 堡垒窗口内未获增益且在堡垒内：优先小幅移动累计驻留，不追逐其他目标。
  if (fortress_active(world) && !peerless_granted_ &&
      in_fortress(world, world.position())) {
    auto move = residency_move(world);
    if (move.has_value()) {
      last_decision_reason =
          "堡垒驻留（天下无双 " + std::to_string(residency_streak_) + "/" +
          std::to_string(kResidencyTicks) + "）";
      return Decision{"move", move};
    }
  }
  // 无即时威胁时按目标优先级推进：资源/任务/追击敌人统一按
  // 价值/距离 加权竞争（敌人权重按血量分档；资源按原始价值参与）
  // → 探索。
  // 射程内的攻击/逃离仍由上层即时战术按当前感知决定，不参与竞争。
  auto move = goal_move(world);
  if (move.has_value()) {
    return Decision{"move", move};
  }

  // 残血无目标：默认继续探索找资源/一炮可杀目标；residual_defend=true 时
  // 退化为防御待机（不原地 wait、不去未知区域冒险）。
  if (world.hp() <= params_.residual_hp && params_.residual_defend) {
    last_decision_reason = "残血无目标，防御待机";
    return Decision{"defend", std::nullopt};
  }

  move = explore_move(world);
  if (move.has_value()) {
    last_decision_reason = "探索未知区域";
    return Decision{"move", move};
  }

  last_decision_reason = "无目标，等待";
  return Decision{"wait", std::nullopt};
}

// ================================================================ 攻击
bool Strategy::attack_allowed(const WorldModel& world, const Entity& target,
                              bool peerless) const {
  if (target.protected_) {
    return false;  // 出生保护 0 伤害且挡住后方
  }
  if (target.defending) {
    // 洁身自好窗口：攻击防御目标 0 伤害且反伤 20，绝对不打。
    if (reflection_active(world)) {
      return false;
    }
    // 攻击不耗弹药、只占一个 Tick：防御目标每炮掉 10 血就是白赚，
    // 不再设自身血量闸门，直接打。
    return params_.chip_defender_enabled;
  }
  // 残局保护：血量过低且无击杀机会时不主动交火（增益期间按增益伤害重算）。
  const int kill_hp = peerless ? kPeerlessDamage : kKillHp;
  if (world.hp() <= 20 && target.hp > kill_hp) {
    return false;
  }
  return true;
}

std::optional<std::string> Strategy::choose_attack(const WorldModel& world) const {
  if (world.protected_now() || world.stage != 2) {
    return std::nullopt;
  }
  std::unordered_map<Cell, Entity> by_cell;
  for (const auto& e : world.last_entities) {
    if (e.type == "robot") {
      by_cell[e.abs] = e;
    }
  }
  const Cell pos = world.position();

  // 天下无双增益：任意合法方向命中横纵 ±2 的 5×5 内全部存活敌人（含斜向、
  // 不受障碍遮挡），方向不限制目标集合。洁身自好窗口内 5×5 只要有防御目标
  // 就整个放弃（范围攻击命中即反伤）。
  if (peerless_active(world)) {
    bool any_defender = false;
    const Entity* nearest = nullptr;
    int nearest_dist = kPeerlessRange + 1;
    for (const auto& e : world.last_entities) {
      if (e.type != "robot") {
        continue;
      }
      const int dist = chebyshev(pos, e.abs);
      if (dist > kPeerlessRange) {
        continue;
      }
      if (e.defending && reflection_active(world)) {
        any_defender = true;
      }
      if (!attack_allowed(world, e, true)) {
        continue;
      }
      if (dist < nearest_dist) {
        nearest_dist = dist;
        nearest = &e;
      }
    }
    if (any_defender || nearest == nullptr) {
      return std::nullopt;
    }
    // 方向不限制目标集合，取最近可攻击目标的主轴方向即可。
    const int dx = nearest->abs.x - pos.x;
    const int dy = nearest->abs.y - pos.y;
    return std::string(std::abs(dx) >= std::abs(dy)
                           ? (dx >= 0 ? "right" : "left")
                           : (dy >= 0 ? "down" : "up"));
  }

  std::optional<std::string> best_direction;
  const Entity* best_target = nullptr;
  double best_score = -1e9;
  for (const auto& direction : kDirections) {
    const Cell delta = dir_delta(std::string(direction));
    const Entity* target = nullptr;
    for (int dist = 1; dist <= 2; ++dist) {
      const auto it =
          by_cell.find(Cell{pos.x + delta.x * dist, pos.y + delta.y * dist});
      if (it != by_cell.end()) {
        target = &it->second;
        break;
      }
    }
    if (target == nullptr || !attack_allowed(world, *target, false)) {
      continue;
    }
    double score = 0.0;
    const int kill_hp = target->defending ? kDefenderKillHp : kKillHp;
    if (target->hp <= kill_hp) {
      score += 100;  // 一炮击杀；击杀奖励按致死批次贡献分配
    }
    if (!target->defending) {
      score += 30;
    } else if (target->hp <= kDefenderKillHp) {
      score += 40;  // 一炮可杀的防御目标；白嫖目标不加分，避免压过普通目标
    }
    score -= target->hp;  // 优先残血
    if (score > best_score) {
      best_score = score;
      best_direction = std::string(direction);
      best_target = target;
    }
  }
  if (best_direction.has_value() && best_target != nullptr) {
    return best_direction;
  }
  return std::nullopt;
}

// ================================================================ 威胁
std::unordered_set<Cell> Strategy::threat_cells(
    const WorldModel& world, const std::vector<Entity>& visible) const {
  // 敌方直线 1–2 格攻击可命中的格子集合。
  // 出生保护中的敌人不能攻击，其火力线不计入威胁。
  std::unordered_set<Cell> cells;
  for (const auto& entity : visible) {
    if (entity.protected_) {
      continue;
    }
    const Cell pos = entity.abs;
    for (int dist = 1; dist <= 2; ++dist) {
      const Cell candidates[4] = {{pos.x + dist, pos.y},
                                  {pos.x - dist, pos.y},
                                  {pos.x, pos.y + dist},
                                  {pos.x, pos.y - dist}};
      for (const Cell& cell : candidates) {
        if (in_bounds(cell, world.map_width, world.map_height)) {
          cells.insert(cell);
        }
      }
    }
  }
  return cells;
}

std::unordered_set<Cell> Strategy::threat_next_cells(
    const WorldModel& world, const std::vector<Entity>& visible) const {
  // 敌方本 Tick 移动一步后（或原地）可命中的格子并集，用于避让代价。
  std::unordered_set<Cell> cells;
  for (const auto& entity : visible) {
    if (entity.protected_) {
      continue;
    }
    const Cell pos = entity.abs;
    const Cell spots[5] = {{pos.x, pos.y},
                           {pos.x + 1, pos.y},
                           {pos.x - 1, pos.y},
                           {pos.x, pos.y + 1},
                           {pos.x, pos.y - 1}};
    for (const Cell& spot : spots) {
      if (!in_bounds(spot, world.map_width, world.map_height)) {
        continue;
      }
      for (int dist = 1; dist <= 2; ++dist) {
        const Cell candidates[4] = {{spot.x + dist, spot.y},
                                    {spot.x - dist, spot.y},
                                    {spot.x, spot.y + dist},
                                    {spot.x, spot.y - dist}};
        for (const Cell& cell : candidates) {
          if (in_bounds(cell, world.map_width, world.map_height)) {
            cells.insert(cell);
          }
        }
      }
    }
  }
  return cells;
}

std::optional<std::string> Strategy::escape_move(
    WorldModel& world, const std::vector<Entity>& visible) {
  // 选择当前不受威胁的相邻格；全部受威胁返回 nullopt（由调用方转为防御）。
  const Cell pos = world.position();
  std::unordered_set<Cell> occupied;
  std::vector<Cell> enemy_positions;
  for (const auto& e : visible) {
    occupied.insert(e.abs);
    enemy_positions.push_back(e.abs);
  }
  const auto threat_now = threat_cells(world, visible);
  const auto threat_next = threat_next_cells(world, visible);

  std::optional<std::pair<std::pair<int, int>, std::string>> best;
  for (const auto& direction : kDirections) {
    const Cell delta = dir_delta(std::string(direction));
    const Cell cell{pos.x + delta.x, pos.y + delta.y};
    if (!in_bounds(cell, world.map_width, world.map_height)) {
      continue;
    }
    if (world.obstacles.count(cell) > 0 || occupied.count(cell) > 0) {
      continue;
    }
    if (threat_now.count(cell) > 0) {
      continue;  // 逃进火力线没有意义
    }
    const int in_next = threat_next.count(cell) > 0 ? 1 : 0;
    int distance = 0;
    if (!enemy_positions.empty()) {
      distance = std::numeric_limits<int>::max();
      for (const Cell& enemy : enemy_positions) {
        distance = std::min(distance, chebyshev(cell, enemy));
      }
    }
    const auto cost = std::make_pair(in_next, -distance);
    if (!best.has_value() || cost < best->first) {
      best = std::make_pair(cost, std::string(direction));
    }
  }
  if (!best.has_value()) {
    return std::nullopt;
  }
  return apply_inversion(world, best->second);
}

// ================================================================ 目标移动
std::optional<std::string> Strategy::goal_move(WorldModel& world) {
  // 资源、reach 任务与追击敌人统一按 价值/距离 评分竞争：
  // 两阶段资源都按原始价值参与（不缩权）；敌人权重按血量分档；
  // 残血只对一炮可杀目标放宽追击半径。天下无双增益期间敌人权重全部
  // 乘倍率，一炮可杀阈值按增益伤害（50/10）重算。
  const bool peerless = peerless_active(world);
  const auto danger = enemy_danger(world);
  std::vector<std::pair<Cell, int>> goals;
  std::unordered_map<Cell, int> weights;
  // 追击资格：血量充足（≥chase_hp_min）时正常追击；残血（≤residual_hp）
  // 只对一炮可杀的目标放宽追击半径，其余目标仅在近距离半径内作战。
  const bool residual = world.hp() <= params_.residual_hp;
  const bool chase_eligible = residual || world.hp() >= params_.chase_hp_min;
  if (world.stage == 2 &&
      (decision_type == "combat" || decision_type == "balanced") &&
      chase_eligible) {
    for (const auto& enemy : world.fresh_enemies()) {
      if (enemy.protected_) {
        continue;  // 出生保护目标不可伤害，不值得追
      }
      if (residual) {
        // 击杀奖励 60 抵得上死亡惩罚 60，一炮可杀的目标才值得放宽半径；
        // 其余目标维持近距离半径，不吃无把握的换血。
        const int kill_hp = enemy.defending
                                ? (peerless ? kPeerlessDefenderDamage
                                            : kDefenderKillHp)
                                : (peerless ? kPeerlessDamage : kKillHp);
        const bool one_shot = enemy.hp <= kill_hp;
        const int radius = one_shot ? params_.chase_radius_residual_kill
                                    : params_.chase_radius_residual;
        if (chebyshev(world.position(), enemy.position) > radius) {
          continue;
        }
      }
      const int base =
          enemy.hp <= 20
              ? params_.enemy_weight_low
              : (enemy.hp <= 40 ? params_.enemy_weight_mid
                                : params_.enemy_weight_high);
      const int weight =
          peerless ? base * params_.peerless_enemy_weight_mult : base;
      auto path = dijkstra(world.position(), enemy.position, world.map_width,
                           world.map_height, world.obstacles, &danger);
      if (!path.has_value() || path->size() < 2) {
        continue;  // 不可达或已相邻（相邻交战由即时战术层处理）
      }
      goals.emplace_back(enemy.position, weight);
      weights[enemy.position] = weight;
    }
  }
  // 资源两阶段都按原始价值参与竞争，不缩权、不设总量封顶；
  // 第二阶段对资源加权（高价值压过中血敌人、普通资源高于满血敌人），
  // 并叠加 LLM goals 偏置。
  const bool stage2 = world.stage == 2;
  for (const auto& [cell, info] : world.resource_cells()) {
    int value = info.value;
    const int age = world.tick - info.last_seen;
    if (age > 30) {
      value = std::max(1, value / 2);  // 久未目击的资源可能已被他人收集
    }
    if (stage2) {
      value += info.value >= 20 ? params_.resource_high_bonus
                                : params_.resource_normal_bonus;
      const std::string goal_type =
          info.value >= 20 ? "resource_high" : "resource_normal";
      value += goal_priority_bonus(world, goal_type);
    }
    goals.emplace_back(cell, value);
    weights[cell] = value;
  }
  if (const Mission* reach = world.reach_mission()) {
    if (reach->position.has_value()) {
      const Cell pos = *reach->position;
      if (pos != world.position()) {
        const int reach_weight =
            params_.reach_weight + goal_priority_bonus(world, "reach_target");
        goals.emplace_back(pos, reach_weight);
        weights[pos] = reach_weight;
      }
    }
  }
  // 天下无双：堡垒窗口内未获增益时把堡垒中心作为前往目标；驻留移动由
  // proceed_toward_goals 负责。
  if (world.stage == 2 && fortress_active(world) && !peerless_granted_) {
    const Cell center{world.map_width / 2, world.map_height / 2};
    if (center != world.position()) {
      goals.emplace_back(center, params_.fortress_goal_weight);
      weights[center] = params_.fortress_goal_weight;
    }
  }
  if (goals.empty()) {
    return std::nullopt;
  }
  // 目标滞回：同一场次连续 Tick 时，保持上一 Tick 选中且仍具竞争力的目标
  const Cell* stick = nullptr;
  if (last_goal_cell_.has_value() &&
      last_goal_session_ == world.session_id &&
      last_goal_tick_ + 1 == world.tick) {
    stick = &*last_goal_cell_;
  }
  auto result = nearest_goal(world.position(), goals, world.map_width,
                             world.map_height, world.obstacles, &danger,
                             &weights, stick);
  if (!result.has_value()) {
    return std::nullopt;
  }
  last_goal_session_ = world.session_id;
  last_goal_tick_ = world.tick;
  last_goal_cell_ = result->target;
  bool is_enemy = false;
  for (const auto& enemy : world.fresh_enemies()) {
    if (enemy.position == result->target) {
      is_enemy = true;
      break;
    }
  }
  const Cell fortress_center{world.map_width / 2, world.map_height / 2};
  if (is_enemy) {
    last_decision_reason =
        peerless ? "追击敌人（天下无双增益）" : "追击敌人（残血优先）";
  } else if (result->target == fortress_center && fortress_active(world) &&
             !peerless_granted_) {
    last_decision_reason = "前往堡垒（天下无双）";
  } else {
    last_decision_reason = "前往资源/任务目标";
  }
  return path_direction(world, result->path);
}

std::optional<std::string> Strategy::explore_move(WorldModel& world) {
  // 探索承诺：同一方向连续保持若干 Tick。前沿的新颖度评分随视野更新
  // 逐 Tick 变化，若每 Tick 重选目标会在两个前沿之间来回摆动。
  const auto danger = enemy_danger(world);
  if (explore_session_ == world.session_id &&
      world.tick < explore_commit_until_ && is_direction(explore_dir_) &&
      !last_move_blocked(world)) {
    const Cell pos = world.position();
    const Cell delta = dir_delta(explore_dir_);
    const Cell next{pos.x + delta.x, pos.y + delta.y};
    if (in_bounds(next, world.map_width, world.map_height) &&
        world.obstacles.count(next) == 0 && !occupied_by_robot(world, next) &&
        danger.count(next) == 0) {
      return apply_inversion(world, explore_dir_);
    }
  }
  const auto known = known_free_set(world.obstacles, world.visited);
  auto candidates = frontier_cells(world.position(), world.map_width,
                                   world.map_height, world.obstacles, known);
  for (const auto& candidate : candidates) {
    if (candidate.path.empty() ||
        occupied_by_robot(world, candidate.path.front())) {
      continue;  // 首选前沿被敌人挡住时顺延到下一个前沿
    }
    if (danger.count(candidate.path.front()) > 0) {
      continue;  // 前沿第一步落入敌方火力线，顺延下一个前沿
    }
    auto direction = direction_to(world.position(), candidate.path.front());
    if (!direction.has_value()) {
      continue;
    }
    explore_session_ = world.session_id;
    explore_dir_ = *direction;
    explore_commit_until_ = world.tick + 3;
    return apply_inversion(world, explore_dir_);
  }
  return std::nullopt;
}

std::optional<std::string> Strategy::move_toward(
    WorldModel& world, const Cell& target,
    const std::unordered_map<Cell, int>& danger) {
  auto path = dijkstra(world.position(), target, world.map_width,
                       world.map_height, world.obstacles, &danger);
  if (!path.has_value()) {
    return std::nullopt;
  }
  return path_direction(world, *path);
}

std::unordered_map<Cell, int> Strategy::enemy_danger(
    const WorldModel& world) const {
  // 敌方下一 Tick 火力覆盖代价图；无目击敌人时为空。任何血量都保留代价，
  // 不拿血量换路程：血量优先留给交战、任务和抢点。
  const std::vector<Entity> visible = visible_robots(world);
  if (visible.empty()) {
    return {};
  }
  const int penalty = danger_penalty(decision_type);
  const auto cells = threat_next_cells(world, visible);
  std::unordered_map<Cell, int> danger;
  danger.reserve(cells.size());
  for (const Cell& cell : cells) {
    danger[cell] = penalty;
  }
  return danger;
}

// ================================================================ 方向换算
bool Strategy::occupied_by_robot(const WorldModel& world,
                                 const Cell& cell) const {
  for (const auto& e : world.last_entities) {
    if (e.type == "robot" && e.abs == cell) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> Strategy::path_direction(
    WorldModel& world, const std::vector<Cell>& path) const {
  if (path.empty()) {
    return std::nullopt;
  }
  // 下一步格被可见敌人占据时无法移动：交还调用方另选动作，避免 E_BLOCKED。
  if (occupied_by_robot(world, path.front())) {
    return std::nullopt;
  }
  auto direction = direction_to(world.position(), path.front());
  if (!direction.has_value()) {
    return std::nullopt;
  }
  return apply_inversion(world, *direction);
}

std::string Strategy::apply_inversion(const WorldModel& world,
                                      const std::string& direction) const {
  // MOVE 命令的反转补偿有两个来源：
  // 1) world.inverted：由动作回执反馈学习（权威，晚于公告生效）；
  // 2) LLM 事件计划声明的幻术窗口：回执学习前的临时补偿，按绝对 Tick 过期。
  // 协议规定 ATTACK 方向不受幻术反转，故本函数只用于 MOVE。
  const bool plan_inverted = plan_invert_ &&
                             plan_session_ == world.session_id &&
                             world.tick <= plan_invert_until_;
  if (world.inverted || plan_inverted) {
    const auto opp = opposite(direction);
    if (opp.has_value()) {
      return *opp;
    }
  }
  return direction;
}

// ================================================================ LLM 触发
void Strategy::maybe_trigger_llm(WorldModel& world, const WorldEvents& events,
                                 LLMClient* client) {
  // 按需触发 LLM 调用：仅第二阶段；额度/鉴权异常后停止。
  // 成功调用达到软上限后只保留公告驱动的调用（余量保护）。
  if (client == nullptr || world.stage != 2) {
    return;
  }
  if (client->quota_exhausted() || client->auth_failed()) {
    return;
  }
  const int tick = world.tick;
  const bool interval_ok =
      tick - client->last_attempt_tick() >= LLM_MIN_INTERVAL;
  // 公告优先：预告只有几 Tick 提前量，公告触发的调用不再等满 5 Tick，
  // 但保留 3 Tick 最小间隔，让相邻 Tick 到达的同一批公告合并进一次调用，
  // 避免每条公告各占一次完整请求（额度保护）。
  const bool announcement_ok = tick - client->last_attempt_tick() >= 3;
  const bool quota_soft =
      client->successes() >= params_.llm_quota_soft_limit;
  if (events.new_announcements) {
    // 公告落在最小间隔内时挂起，等允许调用再补发：事件预告只有几个 Tick
    // 的提前量，不能因为这次没触发就永远丢掉。
    announcement_llm_pending_ = true;
  }
  bool trigger = false;
  if (events.stage2_detected || events.new_session) {
    trigger = true;
  } else if (announcement_llm_pending_ && announcement_ok) {
    trigger = true;
  } else if (!quota_soft &&
             tick - client->last_success_tick() >= LLM_PERIOD && interval_ok) {
    trigger = true;
  } else if (events.respawned && interval_ok) {
    trigger = true;
  } else if (client->successes() == 0 && tick >= 40 && tick % 20 == 0 &&
             interval_ok) {
    // 正式第二阶段每场至少一次成功调用，未成功前周期性重试
    trigger = true;
  }
  if (trigger) {
    try {
      client->request(build_context(world));
      // 任何一次成功入队的调用都带上了最新公告，挂起标记即可清除。
      announcement_llm_pending_ = false;
    } catch (const std::exception& exc) {
      if (log_) {
        log_("warning", std::string("LLM 上下文构造失败: ") + exc.what());
      }
    }
  }
}

json Strategy::build_context(WorldModel& world) {
  // 参考车边界：提示词只含地图尺寸、当前 Tick、本车状态、任务原文和公开
  // 事件公告。视野实体、比分、本地建议与内部摘要都不外传，避免 LLM 与
  // 实时控制环耦合。session_id 仅用于回执校验，不进入提示词。
  json ctx;
  ctx["session_id"] = world.session_id;
  ctx["tick"] = world.tick;
  ctx["width"] = world.map_width;
  ctx["height"] = world.map_height;
  ctx["position"] = json::array({world.position().x, world.position().y});
  ctx["hp"] = world.hp();
  ctx["alive"] = world.alive();
  ctx["defending"] = world.defending();
  ctx["protected"] = world.protected_now();
  ctx["respawn_in"] = world.respawn_in();
  ctx["mission_text"] = world.mission_text;
  ctx["announcements"] = json(world.announcements);
  return ctx;
}

}  // namespace rmtc
