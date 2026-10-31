#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "rmtc_bot_cpp/llm_client.hpp"
#include "rmtc_bot_cpp/world.hpp"

namespace rmtc {

constexpr int ADVICE_MAX_AGE = 12;         // goals 优先级窗口（Tick）：陈旧建议宁可不用
constexpr int LLM_MIN_INTERVAL = 5;        // 距上次尝试的最小间隔（Tick）
constexpr int LLM_PERIOD = 15;             // 周期性复询间隔（Tick）
constexpr int LLM_PLAN_MAX_LIFETIME = 120; // 事件计划绝对 Tick 的本地封顶：不盲信 LLM 自报的超长窗口

// 可调策略参数（decision_config.json 的 params 段；封印后只允许修改参数）。
struct StrategyParams {
  int enemy_weight_low = 50;           // 极危敌人(hp≤20 一炮死)追击权重
  int enemy_weight_mid = 30;           // 低血敌人(21-40 两炮死)追击权重
  int enemy_weight_high = 10;          // 中高血敌人(>40)追击权重
  int resource_high_bonus = 20;        // 第二阶段高价值资源加权（20+20=40，压过中血敌人 30）
  int resource_normal_bonus = 10;      // 第二阶段普通资源加权（10+10=20，高于满血敌人 10）
  int reach_weight = 20;               // reach 任务坐标权重（任务分 5 ≈ 资源 20）
  int llm_goal_weight = 15;            // LLM 目标默认权重（低于任务 20：情报源而非导航员）
  int chase_hp_min = 41;               // 中血及以上(≥41)正常追击；≤40 走残血半径规则
  int residual_hp = 40;                // 残血档位上限（≤40 执行残血规则）
  int chase_radius_residual = 2;       // 残血追击半径（切比雪夫，超出不追）
  int chase_radius_residual_kill = 4;  // 残血时对一炮可杀目标的追击半径（击杀奖励抵消死亡惩罚）
  int llm_quota_soft_limit = 80;       // 成功调用达到该数后只保留公告触发
  bool residual_defend = false;        // 残血无目标时防御待机（false=继续探索找资源/一炮目标）
  bool chip_defender_enabled = true;   // 对防御中的目标直接开火（洁身自好窗口除外）
  int fortress_goal_weight = 45;       // 天下无双堡垒未获增益时的前往权重（高于高价值资源 40）
  int peerless_enemy_weight_mult = 3;  // 增益期间敌人权重倍率（50/30/10 → 150/90/30）
};

struct Decision {
  std::string action = "wait";
  std::optional<std::string> direction;
};

// 发布前最后一道合法性防线：任何情况下不发出已知会被裁判拒绝的动作。
Decision final_guard(const WorldModel& world, const std::string& action,
                     const std::optional<std::string>& direction);

// 决策引擎：第一阶段自主采集、第二阶段资源对抗任务的优先级链。
//
// 硬性合规约束（任何路径都不违反）：
// - 阵亡期间只发 wait；出生保护期内不攻击；第一阶段只发 move/wait；
// - 幻术不解析公告文本：移动方向反转由 world 根据动作回执反馈学习，
//   事件计划的 invert_controls 作为回执学习之外的临时补偿，ATTACK 不反转
//   （协议规定攻击方向不受幻术影响）；
// - 随机事件文本不做任何正则/关键词解析：LLM 建议经 llm_client 校验后在此融合，
//   建议不合法或缺失时完全由本地规则决策。
//
// 决策优先级（第二阶段）：
// 阵亡 → 事件计划（wait/defend/move_to；wait 在即时火力威胁下让位生存）
// → 即时战术（射程内攻击 / 逃离或防御火力线；洁身自好窗口内不攻击
//   防御目标，身处火力线时优先防御反伤）→ goals 高层目标偏置
// → 目标加权竞争：资源/任务/追击敌人统一按 价值/距离 评分
//   （敌人权重按血量分档；第二阶段资源加权：高价值高于中血敌人、普通资源高于满血敌人；残血只对一炮可杀目标放宽半径）
// → 探索（残血无目标默认继续探索；residual_defend=true 时防御待机）→ 等待。
// 事件计划由 LLM 依据公告给出，按绝对 Tick 过期并做本地上限封顶；
// 本地不内置任何事件参数表。第一阶段为：阵亡 → 资源采集 → 探索 → 等待。
class Strategy {
 public:
  using LogFn =
      std::function<void(const std::string& level, const std::string& message)>;

  explicit Strategy(std::string decision_type = "balanced", LogFn log = nullptr);

  std::string decision_type;
  std::string last_decision_reason;

  void set_params(const StrategyParams& params);
  const StrategyParams& params() const { return params_; }

  Decision decide(WorldModel& world, const std::optional<Advice>& advice);

  // 按需触发 LLM 调用：仅第二阶段；额度/鉴权异常后停止。
  void maybe_trigger_llm(WorldModel& world, const WorldEvents& events,
                         LLMClient* client);

  // 组装 LLM 请求上下文；公告原文原样传入，不做任何解析。
  // 不改动任何策略内部状态。
  nlohmann::json build_context(WorldModel& world);

 private:
  LogFn log_;
  StrategyParams params_;

  std::optional<Decision> hard_guard(const WorldModel& world);
  std::optional<Decision> tactical_decide(WorldModel& world);
  std::optional<Advice> fresh_advice(const WorldModel& world,
                                    const std::optional<Advice>& advice) const;
  // 执行 LLM 事件计划；未采纳（无计划/已过期/不可达）返回 nullopt。
  std::optional<Decision> apply_event_plan(WorldModel& world,
                                           const Advice& advice);
  int plan_until_tick(const Advice& advice) const;
  void update_plan_state(const WorldModel& world, const Advice& advice);
  bool last_move_blocked(const WorldModel& world) const;

  // 洁身自好窗口：LLM 从公告中提取的绝对起止 Tick，本地只做场次隔离与封顶；
  // 窗口内攻击防御目标 0 伤害且反伤，故不攻击任何防御目标。
  void update_event_state(const WorldModel& world, const Advice& advice);
  bool reflection_active(const WorldModel& world) const;

  // 天下无双：堡垒窗口由 LLM 从公告提取；驻留连击与增益授予按本地自身状态
  // 逐 Tick 检测，不解析公告文本。
  void update_fortress_state(const WorldModel& world, const Advice& advice);
  void update_residency(WorldModel& world);
  bool fortress_active(const WorldModel& world) const;
  bool peerless_active(const WorldModel& world) const;
  bool in_fortress(const WorldModel& world, const Cell& cell) const;

  // 参考车式高层规划：LLM 的 goals 优先级作为本地加权竞争的轻量偏置，
  // 随建议窗口过期，不改变逐 Tick 动作仍由本地执行的分层。
  void update_goal_priority(const WorldModel& world, const Advice& advice);
  void prune_goal_priority(const WorldModel& world);
  int goal_priority_bonus(const WorldModel& world,
                          const std::string& type) const;

  Decision rule_decide(WorldModel& world);
  Decision stage1_decide(WorldModel& world);
  Decision proceed_toward_goals(WorldModel& world);

  std::optional<std::string> choose_attack(const WorldModel& world) const;
  std::unordered_set<Cell> threat_cells(const WorldModel& world,
                                        const std::vector<Entity>& visible) const;
  std::unordered_set<Cell> threat_next_cells(
      const WorldModel& world, const std::vector<Entity>& visible) const;
  std::optional<std::string> escape_move(WorldModel& world,
                                         const std::vector<Entity>& visible);
  std::optional<std::string> goal_move(WorldModel& world);
  std::optional<std::string> explore_move(WorldModel& world);
  // 堡垒内小幅移动以累计驻留（移动后必须仍在堡垒内），无可行方向返回 nullopt。
  std::optional<std::string> residency_move(WorldModel& world);
  std::optional<std::string> move_toward(
      WorldModel& world, const Cell& target,
      const std::unordered_map<Cell, int>& danger);
  std::unordered_map<Cell, int> enemy_danger(const WorldModel& world) const;
  // 该格是否被本帧可见机器人占据（不可踏入）。
  bool occupied_by_robot(const WorldModel& world, const Cell& cell) const;
  std::optional<std::string> path_direction(WorldModel& world,
                                            const std::vector<Cell>& path) const;
  std::string apply_inversion(const WorldModel& world,
                              const std::string& direction) const;

  // 攻击资格：出生保护、防御目标、自身残血换命闸门。
  // peerless=true 时按天下无双增益伤害（50/10）重算一炮击杀阈值。
  bool attack_allowed(const WorldModel& world, const Entity& target,
                      bool peerless) const;

  // 目标滞回：上一 Tick 选中的目标格（同场次且连续 Tick 时保持，防抖动）
  std::string last_goal_session_;
  int last_goal_tick_ = -1;
  std::optional<Cell> last_goal_cell_;

  // 探索承诺：连续若干 Tick 保持同一探索方向，防前沿评分随视野更新翻转
  std::string explore_session_;
  std::string explore_dir_;
  int explore_commit_until_ = -1;

  // 事件计划场次与幻术窗口（绝对 Tick，本地封顶；仅 MOVE 生效）
  std::string plan_session_;
  bool plan_invert_ = false;
  int plan_invert_until_ = -1;

  // 洁身自好反伤窗口 [from, until]（绝对 Tick，场次隔离）
  std::string event_session_;
  int reflection_from_ = 0;
  int reflection_until_ = -1;

  // 天下无双：堡垒存在窗口 [fortress_from_, fortress_until_]（绝对 Tick，由
  // LLM 从公告提取），驻留连击与增益授予完全按本地自身状态检测（场次隔离）。
  // 增益在堡垒结束 Tick 统一失效，死亡复活不丢失。
  std::string peerless_session_;
  int fortress_from_ = 0;
  int fortress_until_ = -1;
  int residency_streak_ = 0;
  bool peerless_granted_ = false;

  // 公告触发被最小间隔挡下时挂起，等下一次允许调用再补发
  bool announcement_llm_pending_ = false;

  // LLM 高层目标优先级（参考车式 goals），带场次隔离与 Tick 过期
  std::string goal_priority_session_;
  int goal_priority_until_ = -1;
  std::vector<std::string> goal_priority_;
};

}  // namespace rmtc
