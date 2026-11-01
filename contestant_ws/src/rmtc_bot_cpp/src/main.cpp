// ROS 2 节点入口：话题接入、动作发布、LLM 触发与运行时看护。
//
// 本文件是包内唯一依赖 rclcpp 的模块；protocol/world/planner/llm_client/
// strategy 均为纯逻辑层。使用单线程执行器：决策耗时毫秒级，Tick 周期最短
// 100 ms 足够；LLM 请求在独立线程，不阻塞动作窗口。
#include <curl/curl.h>

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "rmtc_bot_cpp/llm_client.hpp"
#include "rmtc_bot_cpp/protocol.hpp"
#include "rmtc_bot_cpp/strategy.hpp"
#include "rmtc_bot_cpp/world.hpp"

using std_msgs::msg::String;
using namespace std::chrono_literals;

namespace rmtc {

namespace {

constexpr const char* kValidSeats[] = {"A", "B", "C", "D"};
constexpr double kPerceptionStaleSeconds = 5.0;
constexpr double kMatchEndSeconds = 15.0;

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return (value != nullptr && value[0] != '\0') ? std::string(value) : fallback;
}

int env_int_or(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return fallback;
  }
  try {
    return std::stoi(value);
  } catch (const std::exception&) {
    return fallback;
  }
}

double env_double_or(const char* name, double fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return fallback;
  }
  try {
    return std::stod(value);
  } catch (const std::exception&) {
    return fallback;
  }
}

rclcpp::QoS volatile_qos() { return rclcpp::QoS(1).reliable().durability_volatile(); }

rclcpp::QoS transient_qos() {
  return rclcpp::QoS(1).reliable().transient_local();
}

rclcpp::QoS event_qos() {
  return rclcpp::QoS(128).reliable().transient_local();
}

std::string default_decision_config_path() {
  try {
    return ament_index_cpp::get_package_share_directory("rmtc_bot_cpp") +
           "/config/decision_config.json";
  } catch (const std::exception&) {
    return std::string("./config/decision_config.json");
  }
}

struct LastPublished {
  int tick = 0;
  std::string session_id;
  std::string payload;
  std::string action;
};

}  // namespace

class ContestantNode : public rclcpp::Node {
 public:
  ContestantNode(const std::string& seat, const std::string& name,
                 const std::string& gateway, const std::string& token,
                 const std::optional<std::string>& model, int max_tokens,
                 long llm_timeout_seconds, const std::string& decision_config)
      : Node("rmtc_bot_cpp"),
        seat_(seat),
        name_(name),
        gateway_(gateway),
        decision_config_(decision_config),
        strategy_("balanced", [this](const std::string& level,
                                     const std::string& message) {
          if (level == "warning") {
            RCLCPP_WARN(get_logger(), "%s", message.c_str());
          } else {
            RCLCPP_INFO(get_logger(), "%s", message.c_str());
          }
        }),
        llm_(gateway, token, model, max_tokens, llm_timeout_seconds,
             [this](const std::string& level, const std::string& message) {
               if (level == "warning") {
                 RCLCPP_WARN(get_logger(), "%s", message.c_str());
               } else {
                 RCLCPP_INFO(get_logger(), "%s", message.c_str());
               }
             }) {
    action_pub_ = create_publisher<String>("/team_" + seat_ + "/action",
                                           volatile_qos());
    registration_pub_ = create_publisher<String>(
        "/team_" + seat_ + "/registration", transient_qos());
    perception_sub_ = create_subscription<String>(
        "/team_" + seat_ + "/perception", volatile_qos(),
        [this](const String::SharedPtr msg) { on_perception(msg); });
    event_text_sub_ = create_subscription<String>(
        "/referee/event_text", event_qos(),
        [this](const String::SharedPtr msg) { on_event_text(msg); });
    mission_sub_ = create_subscription<String>(
        "/referee/mission", transient_qos(),
        [this](const String::SharedPtr msg) { on_mission(msg); });
    registry_sub_ = create_subscription<String>(
        "/referee/team_registry", transient_qos(),
        [this](const String::SharedPtr msg) { on_registry(msg); });

    start_time_ = std::chrono::steady_clock::now();
    next_status_log_ = start_time_ + std::chrono::seconds(30);
    next_no_perception_log_ = start_time_;

    load_decision_config();
    registration_timer_ = create_wall_timer(
        5s, [this]() { publish_registration(); });
    monitor_timer_ = create_wall_timer(1s, [this]() { monitor(); });

    publish_registration();
    llm_.start();
    health_thread_ =
        std::thread([this]() { health_check_once(); });

    RCLCPP_INFO(get_logger(),
                "席位 %s 队名 '%s' 网关 %s（令牌不记录）决策类型 %s 决策配置 %s",
                seat_.c_str(), name_.c_str(), gateway_.c_str(),
                strategy_.decision_type.c_str(), decision_config_.c_str());
  }

  ~ContestantNode() override {
    llm_.stop();
    if (health_thread_.joinable()) {
      health_thread_.join();
    }
  }

 private:
  // ---------------------------------------------------------------- 订阅回调
  void on_perception(const String::SharedPtr msg) {
    last_perception_time_ = std::chrono::steady_clock::now();
    Perception perception;
    std::string err;
    if (!parse_perception(msg->data, perception, err)) {
      world_.note_dropped_frame();
      RCLCPP_WARN(get_logger(), "感知帧解析失败已丢弃: %s", err.c_str());
      return;
    }

    const std::string& session = perception.session_id;
    const int tick = perception.tick;
    const bool switched = session != world_.session_id;
    if (switched) {
      RCLCPP_INFO(get_logger(), "场次切换: %s -> %s",
                  world_.session_id.empty() ? "无" : world_.session_id.c_str(),
                  session.c_str());
      world_.reset(session);
      last_published_.reset();
      match_ended_reported_ = false;
      consecutive_frames_ = 0;
      publish_registration();
    }

    if (tick < world_.tick) {
      return;  // 同一场次早于当前 Tick 的感知直接丢弃
    }
    ++consecutive_frames_;

    WorldEvents events = world_.update(perception);
    if (switched) {
      // 本节点在 update 之前已完成场次切换 reset，update 察觉不到会话变化；
      // 补置 new_session，确保新场次首帧触发一次 LLM 建议（名录多席位时
      // stage 由 set_registry 静默置 2，stage2_detected 也不会置位）。
      events.new_session = true;
    }
    if (switched) {
      RCLCPP_INFO(get_logger(), "新场次 session_id=%s tick=%d 地图 %d×%d",
                  session.c_str(), tick, world_.map_width, world_.map_height);
    }
    if (events.stage2_detected) {
      RCLCPP_INFO(get_logger(), "检测到第二阶段（任务/敌人/多席位）");
    }
    if (events.invalid_actions_increased) {
      RCLCPP_WARN(get_logger(),
                  "裁判记录无效动作增加: %d，最近决策: %s",
                  world_.invalid_actions_seen,
                  strategy_.last_decision_reason.c_str());
    }
    if (pending_announcement_trigger_) {
      pending_announcement_trigger_ = false;
      events.new_announcements = true;
    }

    // 跨场次首帧防误接管：新场次第一帧 tick 必为 0（或有定位标记），
    // 否则等第二帧确认是持续的新场次流，避免上一场积压消息触发动作。
    if (switched && tick > 0 && !perception.self.position_reason.has_value() &&
        consecutive_frames_ < 2) {
      RCLCPP_INFO(get_logger(), "新场次首帧待确认，本 Tick 不发动作");
      return;
    }

    // 同 Tick 重发：重发缓存动作，不重复推进策略，也不重复触发 LLM
    if (events.redelivery && last_published_.has_value() &&
        last_published_->tick == tick &&
        last_published_->session_id == session) {
      action_pub_->publish(String().set__data(last_published_->payload));
      return;
    }

    const auto advice = llm_.latest_advice();
    Decision decision = strategy_.decide(world_, advice);
    decision =
        final_guard(world_, decision.action, decision.direction);
    std::string payload;
    try {
      payload = build_action(tick, session, decision.action,
                             decision.direction);
    } catch (const ProtocolError& exc) {
      RCLCPP_WARN(get_logger(), "动作构造失败已回退等待: %s", exc.what());
      payload = build_action(tick, session, "wait", std::nullopt);
    }
    action_pub_->publish(String().set__data(payload));
    world_.note_command(decision.action, decision.direction, tick);
    last_published_ =
        LastPublished{tick, session, payload, decision.action};
    ++actions_published_;

    // 第二阶段触发 LLM 建议（后台线程，不阻塞）
    strategy_.maybe_trigger_llm(world_, events, &llm_);
  }

  void on_event_text(const String::SharedPtr msg) {
    // 事件公告话题：只原样入记忆（去重），交给 LLM 理解，不做文本解析。
    std::istringstream stream(msg->data);
    std::string line;
    while (std::getline(stream, line)) {
      if (world_.merge_announcement(line)) {
        pending_announcement_trigger_ = true;
      }
    }
  }

  void on_mission(const String::SharedPtr msg) {
    std::string text = msg->data;
    const auto is_space = [](unsigned char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!text.empty() && is_space(text.front())) {
      text.erase(text.begin());
    }
    while (!text.empty() && is_space(text.back())) {
      text.pop_back();
    }
    if (!text.empty() && text != world_.mission_text) {
      RCLCPP_INFO(get_logger(), "任务公告: %s", text.c_str());
    }
  }

  void on_registry(const String::SharedPtr msg) {
    try {
      const auto value = nlohmann::json::parse(msg->data);
      if (value.is_object() && value.contains("teams") &&
          value["teams"].is_array()) {
        std::vector<std::string> teams;
        for (const auto& row : value["teams"]) {
          if (row.is_object() && row.contains("team") &&
              row["team"].is_string()) {
            teams.push_back(row["team"].get<std::string>());
          }
        }
        world_.set_registry(teams);
      }
    } catch (const std::exception&) {
      return;  // 名录非法时忽略，不影响运行
    }
  }

  // ---------------------------------------------------------------- 注册
  void publish_registration() {
    std::string payload;
    try {
      payload = build_registration(seat_, name_);
    } catch (const ProtocolError& exc) {
      RCLCPP_WARN(get_logger(), "队名非法已回退默认名: %s", exc.what());
      payload = build_registration(seat_, "rmtc_bot");
    }
    registration_pub_->publish(String().set__data(payload));
    ++registrations_published_;
  }

  // ---------------------------------------------------------------- 配置
  void load_decision_config() {
    std::string value = "balanced";
    StrategyParams params;
    try {
      std::ifstream stream(decision_config_);
      if (stream.good()) {
        const auto obj = nlohmann::json::parse(stream);
        if (obj.is_object() && obj.contains("decision_type") &&
            obj["decision_type"].is_string()) {
          value = obj["decision_type"].get<std::string>();
        }
        if (obj.is_object() && obj.contains("params") &&
            obj["params"].is_object()) {
          // 仅接受类型匹配的字段，其余忽略：现场热改参数不得中断运行。
          const auto& p = obj["params"];
          if (p.contains("enemy_weight_low") && p["enemy_weight_low"].is_number_integer()) {
            params.enemy_weight_low = p["enemy_weight_low"].get<int>();
          }
          if (p.contains("enemy_weight_mid") && p["enemy_weight_mid"].is_number_integer()) {
            params.enemy_weight_mid = p["enemy_weight_mid"].get<int>();
          }
          if (p.contains("enemy_weight_high") && p["enemy_weight_high"].is_number_integer()) {
            params.enemy_weight_high = p["enemy_weight_high"].get<int>();
          }
          if (p.contains("resource_high_bonus") && p["resource_high_bonus"].is_number_integer()) {
            params.resource_high_bonus = p["resource_high_bonus"].get<int>();
          }
          if (p.contains("resource_normal_bonus") && p["resource_normal_bonus"].is_number_integer()) {
            params.resource_normal_bonus = p["resource_normal_bonus"].get<int>();
          }
          if (p.contains("reach_weight") && p["reach_weight"].is_number_integer()) {
            params.reach_weight = p["reach_weight"].get<int>();
          }
          if (p.contains("llm_goal_weight") && p["llm_goal_weight"].is_number_integer()) {
            params.llm_goal_weight = p["llm_goal_weight"].get<int>();
          }
          if (p.contains("chase_hp_min") && p["chase_hp_min"].is_number_integer()) {
            params.chase_hp_min = p["chase_hp_min"].get<int>();
          }
          if (p.contains("residual_hp") && p["residual_hp"].is_number_integer()) {
            params.residual_hp = p["residual_hp"].get<int>();
          }
          if (p.contains("chase_radius_residual") && p["chase_radius_residual"].is_number_integer()) {
            params.chase_radius_residual = p["chase_radius_residual"].get<int>();
          }
          if (p.contains("chase_radius_residual_kill") &&
              p["chase_radius_residual_kill"].is_number_integer()) {
            params.chase_radius_residual_kill =
                p["chase_radius_residual_kill"].get<int>();
          }
          if (p.contains("llm_quota_soft_limit") && p["llm_quota_soft_limit"].is_number_integer()) {
            params.llm_quota_soft_limit = p["llm_quota_soft_limit"].get<int>();
          }
          if (p.contains("residual_defend") && p["residual_defend"].is_boolean()) {
            params.residual_defend = p["residual_defend"].get<bool>();
          }
          if (p.contains("chip_defender_enabled") && p["chip_defender_enabled"].is_boolean()) {
            params.chip_defender_enabled = p["chip_defender_enabled"].get<bool>();
          }
          if (p.contains("fortress_goal_weight") && p["fortress_goal_weight"].is_number_integer()) {
            params.fortress_goal_weight = p["fortress_goal_weight"].get<int>();
          }
          if (p.contains("peerless_enemy_weight_mult") && p["peerless_enemy_weight_mult"].is_number_integer()) {
            params.peerless_enemy_weight_mult = p["peerless_enemy_weight_mult"].get<int>();
          }
        }
      }
    } catch (const std::exception&) {
      value = "balanced";
      params = StrategyParams{};
    }
    if (value != "collect" && value != "combat" && value != "balanced") {
      RCLCPP_WARN(get_logger(),
                  "decision_config.json 中 decision_type 非法，沿用 balanced");
      value = "balanced";
    }
    strategy_.decision_type = value;
    strategy_.set_params(params);
  }

  // ---------------------------------------------------------------- 看护
  void health_check_once() {
    const std::string status = LLMClient::check_health(gateway_);
    RCLCPP_INFO(get_logger(), "AI 网关 healthz: %s", status.c_str());
  }

  void monitor() {
    // 决策类型热切换：现场仅允许修改 decision_type（每秒读取一次）
    const std::string previous = strategy_.decision_type;
    load_decision_config();
    if (strategy_.decision_type != previous) {
      RCLCPP_INFO(get_logger(), "决策类型切换: %s -> %s", previous.c_str(),
                  strategy_.decision_type.c_str());
    }

    const auto now = std::chrono::steady_clock::now();
    if (world_.session_id.empty()) {
      if (now - start_time_ > 5s && now >= next_no_perception_log_) {
        next_no_perception_log_ = now + 10s;
        RCLCPP_WARN(get_logger(),
                    "尚未收到可用感知（丢弃帧=%d）：确认 ROS_DOMAIN_ID 与裁判一致、"
                    "裁判已开局",
                    world_.frames_dropped);
      }
      return;
    }
    if (!last_perception_time_.has_value()) {
      return;
    }
    const double idle =
        std::chrono::duration<double>(now - *last_perception_time_).count();
    if (idle > kMatchEndSeconds && !match_ended_reported_) {
      match_ended_reported_ = true;
      log_summary();
    } else if (kPerceptionStaleSeconds < idle && idle <= kMatchEndSeconds &&
               static_cast<int>(idle) % 10 == 0) {
      RCLCPP_WARN(get_logger(), "%.0f 秒未收到感知（暂停或已结束）", idle);
    } else if (idle <= kPerceptionStaleSeconds && now >= next_status_log_) {
      next_status_log_ = now + 30s;
      RCLCPP_INFO(get_logger(),
                  "运行中 tick=%d 位置=[%d, %d] HP=%d 存活=%s 资源价值=%.0f "
                  "有效伤害=%.0f 动作已发=%d",
                  world_.tick, world_.position().x, world_.position().y,
                  world_.hp(), world_.alive() ? "是" : "否",
                  world_.score.resource_value, world_.score.effective_damage,
                  actions_published_);
    }
  }

  void log_summary() {
    RCLCPP_INFO(get_logger(),
                "本场结束汇总: tick=%d 位置=[%d, %d] HP=%d 资源价值=%.0f "
                "有效伤害=%.0f 任务完成=%d 无效动作=%d 动作发布=%d 帧丢弃=%d "
                "LLM 尝试=%d 成功=%d 建议采纳=%d 额度耗尽=%s",
                world_.tick, world_.position().x, world_.position().y,
                world_.hp(), world_.score.resource_value,
                world_.score.effective_damage, world_.score.mission_completed,
                world_.score.invalid_actions, actions_published_,
                world_.frames_dropped, llm_.attempts(), llm_.successes(),
                llm_.advice_count(),
                llm_.quota_exhausted() ? "是" : "否");
  }

  std::string seat_;
  std::string name_;
  std::string gateway_;
  std::string decision_config_;

  WorldModel world_;
  Strategy strategy_;
  LLMClient llm_;

  rclcpp::Publisher<String>::SharedPtr action_pub_;
  rclcpp::Publisher<String>::SharedPtr registration_pub_;
  rclcpp::Subscription<String>::SharedPtr perception_sub_;
  rclcpp::Subscription<String>::SharedPtr event_text_sub_;
  rclcpp::Subscription<String>::SharedPtr mission_sub_;
  rclcpp::Subscription<String>::SharedPtr registry_sub_;
  rclcpp::TimerBase::SharedPtr registration_timer_;
  rclcpp::TimerBase::SharedPtr monitor_timer_;

  std::thread health_thread_;
  std::optional<LastPublished> last_published_;
  std::optional<std::chrono::steady_clock::time_point> last_perception_time_;
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point next_status_log_;
  std::chrono::steady_clock::time_point next_no_perception_log_;
  bool match_ended_reported_ = true;
  bool pending_announcement_trigger_ = false;
  int actions_published_ = 0;
  int registrations_published_ = 0;
  int consecutive_frames_ = 0;
};

}  // namespace rmtc

namespace {

// 解析席位与队名位置参数；忽略 ROS 2 重映射参数。
void parse_argv(int argc, char** argv, std::string& seat, std::string& name,
                std::string& config) {
  bool skip_next = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ros-args") {
      break;
    }
    if (skip_next) {
      skip_next = false;
      continue;
    }
    if (arg == "--decision-config" && i + 1 < argc) {
      config = argv[++i];
      continue;
    }
    if (arg.rfind("--decision-config=", 0) == 0) {
      config = arg.substr(std::string("--decision-config=").size());
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      continue;
    }
    if (seat.empty()) {
      seat = arg;
    } else if (name.empty()) {
      name = arg;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  std::string seat_arg;
  std::string name_arg;
  std::string config_arg;
  parse_argv(argc, argv, seat_arg, name_arg, config_arg);

  std::string seat =
      !seat_arg.empty() ? seat_arg : rmtc::env_or("RMTC_TEAM", "");
  std::transform(seat.begin(), seat.end(), seat.begin(),
                 [](unsigned char c) { return static_cast<char>(::toupper(c)); });
  bool seat_valid = false;
  for (const char* valid : rmtc::kValidSeats) {
    if (seat == valid) {
      seat_valid = true;
      break;
    }
  }
  if (!seat_valid) {
    std::fprintf(
        stderr,
        "席位须为 A/B/C/D（收到 '%s'）。用法: rmtc_bot_cpp <席位> [队名] "
        "--decision-config <路径>；环境变量 RMTC_TEAM 也可设置席位。\n",
        seat.c_str());
    curl_global_cleanup();
    return 2;
  }
  const std::string name =
      !name_arg.empty() ? name_arg : rmtc::env_or("RMTC_TEAM_NAME", "rmtc_bot");
  // RMTC_AI_URL 是 RMTC_AI_GATEWAY 的别名（两者都可写 base 或完整端点）；
  // 同时设置时以 RMTC_AI_GATEWAY 为准。
  std::string gateway = rmtc::env_or("RMTC_AI_GATEWAY", "");
  if (gateway.empty()) {
    gateway = rmtc::env_or("RMTC_AI_URL", "");
  }
  if (gateway.empty()) {
    gateway = "http://127.0.0.1:8080";
  }
  const std::string token =
      rmtc::env_or("RMTC_TEAM_TOKEN", "practice-" + seat);
  const std::string model_env = rmtc::env_or("RMTC_AI_MODEL", "");
  const std::optional<std::string> model =
      model_env.empty() ? std::nullopt
                        : std::optional<std::string>(model_env);
  const int max_tokens = rmtc::env_int_or("RMTC_AI_MAX_TOKENS", 600);
  const double llm_timeout = rmtc::env_double_or("RMTC_AI_TIMEOUT", 30.0);
  std::string decision_config;
  if (!config_arg.empty()) {
    decision_config = config_arg;
  } else {
    const std::string env_config = rmtc::env_or("RMTC_DECISION_CONFIG", "");
    decision_config = env_config.empty()
                          ? rmtc::default_decision_config_path()
                          : env_config;
  }

  rclcpp::init(argc, argv);
  auto node = std::make_shared<rmtc::ContestantNode>(
      seat, name, gateway, token, model, max_tokens,
      static_cast<long>(llm_timeout), decision_config);
  rclcpp::spin(node);
  node.reset();
  rclcpp::shutdown();
  curl_global_cleanup();
  return 0;
}
