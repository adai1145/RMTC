#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "rmtc_bot_cpp/types.hpp"

namespace rmtc {

// 官方 LLM 网关客户端（C++ / libcurl）。
//
// 职责边界（合规）：
// - 把自然语言随机事件公告（event_text）原样交给 LLM 理解，本模块绝不对公告
//   文本做正则/关键词解析；
// - 校验模型输出 JSON（goals 枚举、事件计划 mode/target/until_tick），
//   不合法的输出整条丢弃并降级到本地启发式；
// - 后台线程单在途请求（远低于每令牌 2 个在途上限），latest-wins 单槽队列，
//   绝不阻塞 Tick 决策。

// 事件计划：LLM 依据公开事件公告给出的短时高层指令，按绝对 Tick 过期。
// 本地不解析公告文本，也不内置任何事件参数；计划本身同样要过本地校验。
struct EventPlan {
  std::string mode = "normal";  // normal | wait | defend | move_to
  std::optional<Cell> target;   // move_to 必填，本场坐标系
  int until_tick = 0;           // 绝对 Tick，策略层另做本地上限封顶
  bool invert_controls = false;  // 幻术窗口：MOVE 方向需反转
};

// LLM 建议只做高层规划，逐 Tick 动作始终由本地策略结合最新感知执行。
// goals、事件计划与洁身自好窗口都为空的建议视为无信息，直接丢弃。
struct Advice {
  std::string session_id;
  int tick = 0;
  // 高层目标优先级：resource_high / resource_normal / reach_target。
  std::vector<std::string> goals;
  std::optional<EventPlan> event_plan;
  // 洁身自好窗口 [from, until]（绝对 Tick，均含端点）；until=0 表示无窗口。
  // 公告只由 LLM 理解：本地不解析公告文本，只接收已校验的绝对 Tick。
  // from=0 表示公告未给开始 Tick（按收到建议的 Tick 立即生效）。
  int reflection_from = 0;
  int reflection_until = 0;
  // 天下无双堡垒存在窗口 [from, until]（绝对 Tick，均含端点）；until=0 表示
  // 无窗口。驻留达标与增益授予由本地按自身状态检测，LLM 只负责从公告提取窗口。
  int fortress_from = 0;
  int fortress_until = 0;
  std::string reasoning;
};

// 系统提示词：要求只输出 JSON 建议对象。
std::string system_prompt();

// 从模型输出中提取 JSON 对象（容忍围栏与前后缀文字）。
std::optional<nlohmann::json> extract_json_object(const std::string& text);

// 严格校验模型输出对象；不合法返回 nullopt（由本地规则引擎兜底）。
std::optional<Advice> validate_advice(const nlohmann::json& obj,
                                      const std::string& session_id, int tick);

// 解析 Chat Completions 响应体。成功返回 Advice，否则 nullopt。
std::optional<Advice> parse_response(const nlohmann::json& payload,
                                     const std::string& session_id, int tick);

// 构造用户提示词。context 由 strategy 组装，公告原文原样包含。
std::string build_prompt(const nlohmann::json& context);

class LLMClient {
 public:
  using LogFn =
      std::function<void(const std::string& level, const std::string& message)>;

  LLMClient(std::string gateway, std::string token,
            std::optional<std::string> model = std::nullopt,
            int max_tokens = 600, long timeout_seconds = 30, LogFn log = nullptr);
  ~LLMClient();
  LLMClient(const LLMClient&) = delete;
  LLMClient& operator=(const LLMClient&) = delete;

  void start();
  void stop();

  bool busy() const;

  // 提交一次请求（latest-wins，覆盖未发出的旧请求）。
  void request(const nlohmann::json& context);

  std::optional<Advice> latest_advice();
  void clear_advice();

  int attempts() const { return attempts_.load(); }
  int successes() const { return successes_.load(); }
  int advice_count() const { return advice_count_.load(); }
  bool quota_exhausted() const { return quota_exhausted_.load(); }
  bool auth_failed() const { return auth_failed_.load(); }
  std::string last_error() const;
  int last_attempt_tick() const { return last_attempt_tick_.load(); }
  int last_success_tick() const { return last_success_tick_.load(); }

  // GET /healthz；返回状态描述字符串（仅诊断，失败不影响运行）。
  static std::string check_health(const std::string& gateway,
                                  long timeout_seconds = 3);

 private:
  struct PostResult {
    long status = 0;
    bool ok = false;      // 收到 HTTP 响应（含 4xx/5xx）
    std::string body;
    std::string error;
  };

  void run();
  void call_with_retry(const std::string& prompt,
                       const nlohmann::json& context);
  PostResult post(const std::string& prompt);
  void record_error(const std::string& message);
  // 可被 stop() 打断的等待；返回 true 表示已请求停止。
  bool wait_for(long milliseconds);

  std::string gateway_;
  std::string token_;
  std::optional<std::string> model_;
  int max_tokens_;
  long timeout_seconds_;
  LogFn log_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::optional<nlohmann::json> pending_;
  std::optional<Advice> advice_;
  std::string last_error_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<int> attempts_{0};
  std::atomic<int> successes_{0};
  std::atomic<int> advice_count_{0};
  std::atomic<bool> quota_exhausted_{false};
  std::atomic<bool> auth_failed_{false};
  std::atomic<int> last_attempt_tick_{-1};
  std::atomic<int> last_success_tick_{-1};
};

}  // namespace rmtc
