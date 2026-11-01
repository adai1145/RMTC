#include "rmtc_bot_cpp/llm_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>
#include <utility>

using nlohmann::json;

namespace rmtc {

namespace {
constexpr size_t kMaxRequestBytes = 2u * 1024u * 1024u;  // 网关请求体上限
constexpr std::string_view kChatSuffix = "/v1/chat/completions";

// 网关既可写成 base（http://host:port），也可写成完整端点（.../v1/chat/completions）。
// 统一归一化成 base，post 时只拼一次，避免双重路径 404。
std::string normalize_gateway(std::string value) {
  const auto strip_slash = [&value] {
    while (!value.empty() && value.back() == '/') {
      value.pop_back();
    }
  };
  strip_slash();
  if (value.size() > kChatSuffix.size() &&
      value.compare(value.size() - kChatSuffix.size(), kChatSuffix.size(),
                    kChatSuffix) == 0) {
    value.erase(value.size() - kChatSuffix.size());
    strip_slash();
  }
  return value;
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  const size_t total = size * nmemb;
  static_cast<std::string*>(userdata)->append(ptr, total);
  return total;
}

std::string error_code_of(const std::string& body) {
  try {
    const json obj = json::parse(body);
    if (obj.is_object() && obj.contains("error") && obj["error"].is_object() &&
        obj["error"]["code"].is_string()) {
      return obj["error"]["code"].get<std::string>();
    }
  } catch (const json::exception&) {
  }
  return "";
}

}  // namespace

std::string system_prompt() {
  return
      "你是机甲大师图灵对决赛中一辆战车的战术顾问。你只输出高层建议，"
      "每 Tick 的具体动作由本地程序结合最新感知执行并做合法性校验。\n"
      "输入只有地图尺寸、当前 Tick、本车状态、任务原文和公开的随机事件公告。"
      "公告是自然语言，事件的时间、坐标、持续时长、数值一律以公告原文为准；"
      "公告没有给出或你没把握的信息不要编造，也不要套用固定参数。\n"
      "只输出一个 JSON 对象，不要输出任何其他文字或 Markdown 标记，格式：\n"
      "{\"goals\": [\"resource_high\"|\"resource_normal\"|\"reach_target\", ...],\n"
      " \"event_plan\": {\"mode\": \"normal\"|\"wait\"|\"defend\"|\"move_to\",\n"
      "                \"target\": [x, y],\n"
      "                \"until_tick\": 整数,\n"
      "                \"invert_controls\": true|false},\n"
      " \"reflection_from\": 整数或 null,\n"
      " \"reflection_until\": 整数或 null,\n"
      " \"fortress_from\": 整数或 null,\n"
      " \"fortress_until\": 整数或 null,\n"
      " \"reasoning\": \"简要理由\"}\n"
      "字段规则：\n"
      "- goals 按优先级排列，仅用于目标偏好；没有偏好时给空数组。"
      "resource_normal=普通资源，resource_high=高价值资源，"
      "reach_target=任务要求到达的坐标。\n"
      "- event_plan 可为 null。mode=move_to 时必须给出本场地图内的绝对坐标 "
      "target；until_tick 是计划失效的绝对 Tick（0-5000），到期后本地自动回退"
      "到常规规则。\n"
      "- 没有需要应对的事件时 event_plan 填 null；不要为了凑字段编造事件。\n"
      "- invert_controls 仅在公告明确移动方向会反转时填 true。\n"
      "- 「洁身自好」：预告或生效公告都要利用。reflection_from/until 填反弹"
      "效果起止的绝对 Tick（均含，0-5000）。预告「Tick X 起，持续 D」时填 "
      "from=X、until=X+D-1；生效公告直接给出结束 Tick 时 from 填当前 Tick "
      "或 null。窗口内本地不攻击任何防御中的敌人；事件已结束或无法推算结束 "
      "Tick 时两项填 null，不要编造。\n"
      "- 「天下无双」：公告给出地图中心堡垒的生成与持续时长时，fortress_from/"
      "until 填堡垒存在窗口，起止推算规则与洁身自好相同（from=X、until=X+D-1，"
      "均含，0-5000）。堡垒本身无需 event_plan，本地自动前往驻留；无法推算"
      "时填 null。\n"
      "- 公告给出爆炸、辣椒火线等伤害区域时：无论 HP 多少都不要停留或穿越；"
      "已在区域内就用 move_to 指向区域外安全格，不要用 wait/defend 留在伤害"
      "区内。血量优先留给交战、任务和抢点。\n"
      "goals、event_plan、洁身自好窗口与天下无双堡垒窗口至少一项有实际内容，"
      "否则本地会丢弃整条建议。";
}

std::optional<json> extract_json_object(const std::string& text) {
  std::string trimmed = text;
  const auto is_space = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!trimmed.empty() && is_space(trimmed.front())) {
    trimmed.erase(trimmed.begin());
  }
  while (!trimmed.empty() && is_space(trimmed.back())) {
    trimmed.pop_back();
  }
  try {
    const json obj = json::parse(trimmed);
    if (obj.is_object()) {
      return obj;
    }
  } catch (const json::exception&) {
  }
  const size_t start = trimmed.find('{');
  const size_t end = trimmed.rfind('}');
  if (start == std::string::npos || end <= start) {
    return std::nullopt;
  }
  try {
    const json obj = json::parse(trimmed.substr(start, end - start + 1));
    if (obj.is_object()) {
      return obj;
    }
  } catch (const json::exception&) {
  }
  return std::nullopt;
}

std::optional<Advice> validate_advice(const json& obj,
                                      const std::string& session_id, int tick) {
  if (!obj.is_object()) {
    return std::nullopt;
  }
  Advice advice;
  advice.session_id = session_id;
  advice.tick = tick;

  // goals：只接受三个精确枚举，按优先级去重保序；未知词视为整条建议非法，
  // 避免模型用同义词悄悄改变语义后本地仍照单执行。
  if (obj.contains("goals") && !obj["goals"].is_null()) {
    const auto& goals = obj["goals"];
    if (!goals.is_array()) {
      return std::nullopt;
    }
    for (const auto& item : goals) {
      if (!item.is_string()) {
        return std::nullopt;
      }
      const std::string goal = item.get<std::string>();
      if (goal != "resource_high" && goal != "resource_normal" &&
          goal != "reach_target") {
        return std::nullopt;
      }
      if (std::find(advice.goals.begin(), advice.goals.end(), goal) ==
          advice.goals.end()) {
        advice.goals.push_back(goal);
      }
    }
  }

  // event_plan：可空；出现即必须结构完整，否则整条建议丢弃。
  if (obj.contains("event_plan") && !obj["event_plan"].is_null()) {
    const auto& plan_json = obj["event_plan"];
    if (!plan_json.is_object()) {
      return std::nullopt;
    }
    EventPlan plan;
    if (!plan_json.contains("mode") || !plan_json["mode"].is_string()) {
      return std::nullopt;
    }
    plan.mode = plan_json["mode"].get<std::string>();
    if (plan.mode != "normal" && plan.mode != "wait" && plan.mode != "defend" &&
        plan.mode != "move_to") {
      return std::nullopt;
    }
    const bool until_present = plan_json.contains("until_tick") &&
                               !plan_json["until_tick"].is_null();
    if (plan.mode != "normal" && !until_present) {
      return std::nullopt;  // 非 normal 计划必须有绝对失效 Tick
    }
    if (until_present) {
      if (!plan_json["until_tick"].is_number_integer()) {
        return std::nullopt;
      }
      plan.until_tick = plan_json["until_tick"].get<int>();
      if (plan.until_tick < 0 || plan.until_tick > 5000) {
        return std::nullopt;  // 参考车协议上限：绝对 Tick 不超过 5000
      }
    }
    if (plan.mode == "move_to") {
      if (!plan_json.contains("target") || !plan_json["target"].is_array() ||
          plan_json["target"].size() != 2 ||
          !plan_json["target"][0].is_number_integer() ||
          !plan_json["target"][1].is_number_integer()) {
        return std::nullopt;
      }
      const int x = plan_json["target"][0].get<int>();
      const int y = plan_json["target"][1].get<int>();
      if (x < 0 || y < 0) {
        return std::nullopt;
      }
      plan.target = Cell{x, y};
    }
    if (plan_json.contains("invert_controls") &&
        !plan_json["invert_controls"].is_null()) {
      if (!plan_json["invert_controls"].is_boolean()) {
        return std::nullopt;
      }
      plan.invert_controls = plan_json["invert_controls"].get<bool>();
    }
    advice.event_plan = plan;
  }

  // reflection_from / reflection_until：洁身自好窗口的绝对起止 Tick（均含）；
  // 类型错误整条丢弃，范围与 until_tick 一致（0-5000），until=0 表示无窗口。
  if (obj.contains("reflection_from") && !obj["reflection_from"].is_null()) {
    if (!obj["reflection_from"].is_number_integer()) {
      return std::nullopt;
    }
    advice.reflection_from = obj["reflection_from"].get<int>();
    if (advice.reflection_from < 0 || advice.reflection_from > 5000) {
      return std::nullopt;
    }
  }
  if (obj.contains("reflection_until") && !obj["reflection_until"].is_null()) {
    if (!obj["reflection_until"].is_number_integer()) {
      return std::nullopt;
    }
    advice.reflection_until = obj["reflection_until"].get<int>();
    if (advice.reflection_until < 0 || advice.reflection_until > 5000) {
      return std::nullopt;
    }
  }
  if (advice.reflection_from > 0 && advice.reflection_until > 0 &&
      advice.reflection_from > advice.reflection_until) {
    return std::nullopt;  // 起止颠倒的窗口不可执行
  }

  // fortress_from / fortress_until：天下无双堡垒存在窗口的绝对起止 Tick（均含）；
  // 校验规则与洁身自好窗口一致，until=0 表示无窗口。
  if (obj.contains("fortress_from") && !obj["fortress_from"].is_null()) {
    if (!obj["fortress_from"].is_number_integer()) {
      return std::nullopt;
    }
    advice.fortress_from = obj["fortress_from"].get<int>();
    if (advice.fortress_from < 0 || advice.fortress_from > 5000) {
      return std::nullopt;
    }
  }
  if (obj.contains("fortress_until") && !obj["fortress_until"].is_null()) {
    if (!obj["fortress_until"].is_number_integer()) {
      return std::nullopt;
    }
    advice.fortress_until = obj["fortress_until"].get<int>();
    if (advice.fortress_until < 0 || advice.fortress_until > 5000) {
      return std::nullopt;
    }
  }
  if (advice.fortress_from > 0 && advice.fortress_until > 0 &&
      advice.fortress_from > advice.fortress_until) {
    return std::nullopt;  // 起止颠倒的窗口不可执行
  }

  const bool has_goals = !advice.goals.empty();
  const bool has_plan =
      advice.event_plan.has_value() && advice.event_plan->mode != "normal";
  const bool has_reflection = advice.reflection_until > 0;
  const bool has_fortress = advice.fortress_until > 0;
  if (!has_goals && !has_plan && !has_reflection && !has_fortress) {
    return std::nullopt;  // 无任何可执行信息，交给本地规则
  }

  if (obj.contains("reasoning") && obj["reasoning"].is_string()) {
    advice.reasoning = obj["reasoning"].get<std::string>();
    if (advice.reasoning.size() > 500) {
      advice.reasoning.resize(500);
    }
  }
  return advice;
}

std::optional<Advice> parse_response(const json& payload,
                                     const std::string& session_id, int tick) {
  if (!payload.is_object()) {
    return std::nullopt;
  }
  std::optional<json> obj;
  const auto& choices = payload.value("choices", json(nullptr));
  if (choices.is_array() && !choices.empty() && choices[0].is_object()) {
    const auto& message = choices[0].value("message", json(nullptr));
    if (message.is_object()) {
      const auto& content = message.value("content", json(nullptr));
      if (content.is_string()) {
        obj = extract_json_object(content.get<std::string>());
      } else if (content.is_object()) {
        obj = content;
      }
    }
  }
  if (!obj.has_value() &&
      (payload.contains("goals") || payload.contains("event_plan") ||
       payload.contains("reflection_from") ||
       payload.contains("reflection_until") ||
       payload.contains("fortress_from") ||
       payload.contains("fortress_until"))) {
    obj = payload;  // 防御性兼容：响应体直接是建议对象
  }
  if (!obj.has_value()) {
    return std::nullopt;
  }
  return validate_advice(*obj, session_id, tick);
}

std::string build_prompt(const json& context) {
  // 只提供地图尺寸、当前 Tick、本车状态、任务原文与公开事件公告：
  // 局部视野实体与本地评分不进入提示词，避免 LLM 与实时控制环耦合。
  std::ostringstream out;
  const int tick = context.value("tick", 0);
  out << "当前情况（公开信息，Tick " << tick << "）：\n";
  out << "- 地图尺寸: " << context.value("width", 0) << " × "
      << context.value("height", 0) << "\n";
  out << "- 本车状态: 位置 "
      << context.value("position", json::array()).dump() << "，HP "
      << context.value("hp", 0) << "，存活: "
      << (context.value("alive", true) ? "是" : "否") << "，防御: "
      << (context.value("defending", false) ? "是" : "否") << "，出生保护: "
      << (context.value("protected", false) ? "是" : "否") << "，复活等待: "
      << context.value("respawn_in", 0) << "\n";
  const std::string mission_text = context.value("mission_text", std::string());
  out << "- 任务原文: " << (mission_text.empty() ? "（无）" : mission_text)
      << "\n";
  const auto& announcements = context.value("announcements", json::array());
  if (announcements.empty()) {
    out << "- 公开事件公告：（无）\n";
  } else {
    out << "- 公开事件公告（官方原文，时间/坐标/数值以原文为准）：\n";
    for (auto it = announcements.rbegin(); it != announcements.rend(); ++it) {
      out << "  " << it->get<std::string>() << "\n";
    }
  }
  out << "请输出建议 JSON。";
  return out.str();
}

LLMClient::LLMClient(std::string gateway, std::string token,
                     std::optional<std::string> model, int max_tokens,
                     long timeout_seconds, LogFn log)
    : gateway_(std::move(gateway)),
      token_(std::move(token)),
      model_(std::move(model)),
      max_tokens_(max_tokens),
      timeout_seconds_(timeout_seconds),
      log_(std::move(log)) {
  gateway_ = normalize_gateway(gateway_);
}

LLMClient::~LLMClient() { stop(); }

void LLMClient::start() {
  if (worker_.joinable()) {
    return;
  }
  stop_.store(false);
  worker_ = std::thread(&LLMClient::run, this);
}

void LLMClient::stop() {
  stop_.store(true);
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool LLMClient::busy() const {
  std::lock_guard<std::mutex> lock(mu_);
  return pending_.has_value();
}

void LLMClient::request(const json& context) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_ = context;
  }
  cv_.notify_one();
}

std::optional<Advice> LLMClient::latest_advice() {
  std::lock_guard<std::mutex> lock(mu_);
  return advice_;
}

void LLMClient::clear_advice() {
  std::lock_guard<std::mutex> lock(mu_);
  advice_.reset();
}

std::string LLMClient::last_error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_error_;
}

void LLMClient::record_error(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_ = message;
  }
  if (log_) {
    log_("warning", message);
  }
}

bool LLMClient::wait_for(long milliseconds) {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                      [this] { return stop_.load(); });
}

void LLMClient::run() {
  while (!stop_.load()) {
    json context;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_.load() || pending_.has_value(); });
      if (stop_.load()) {
        return;
      }
      context = std::move(*pending_);
      pending_.reset();
    }
    std::string prompt;
    try {
      prompt = build_prompt(context);
    } catch (const std::exception& exc) {
      // 提示词构造失败不应杀死线程
      record_error(std::string("提示词构造失败: ") + exc.what());
      continue;
    }
    call_with_retry(prompt, context);
  }
}

void LLMClient::call_with_retry(const std::string& prompt,
                                const json& context) {
  const std::string session_id = context.value("session_id", std::string());
  const int tick = context.value("tick", 0);
  for (int attempt = 0; attempt < 3; ++attempt) {  // 首次 + 2 次限流退避重试
    attempts_.fetch_add(1);
    last_attempt_tick_.store(tick);
    PostResult result;
    try {
      result = post(prompt);
    } catch (const std::exception& exc) {
      record_error(std::string("网关请求构造异常: ") + exc.what());
      return;
    }
    if (!result.ok) {
      record_error("网关调用失败（" + result.error + "）");
      return;
    }
    if (result.status == 200) {
      json payload;
      try {
        payload = json::parse(result.body);
      } catch (const json::exception&) {
        record_error("网关返回 200 但响应不是 JSON");
        return;
      }
      if (!payload.is_object() || !payload.contains("choices") ||
          !payload["choices"].is_array() || payload["choices"].empty()) {
        record_error("网关返回 200 但缺少 choices");
        return;
      }
      successes_.fetch_add(1);
      last_success_tick_.store(tick);
      std::optional<Advice> advice;
      try {
        advice = parse_response(payload, session_id, tick);
      } catch (const std::exception& exc) {
        record_error(std::string("LLM 响应解析异常: ") + exc.what());
        return;
      }
      if (advice.has_value()) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          advice_ = *advice;
        }
        advice_count_.fetch_add(1);
        if (log_) {
          std::string detail = "goals";
          if (advice->goals.empty()) {
            detail += "（空）";
          } else {
            for (const auto& goal : advice->goals) {
              detail += " " + goal;
            }
          }
          if (advice->event_plan.has_value()) {
            detail += " plan=" + advice->event_plan->mode;
            if (advice->event_plan->target.has_value()) {
              detail +=
                  " [" + std::to_string(advice->event_plan->target->x) + ", " +
                  std::to_string(advice->event_plan->target->y) + "]";
            }
            detail += " until=" +
                      std::to_string(advice->event_plan->until_tick);
          }
          if (advice->reflection_until > 0) {
            detail += " reflect=" +
                      std::to_string(advice->reflection_from > 0
                                         ? advice->reflection_from
                                         : tick) +
                      ".." + std::to_string(advice->reflection_until);
          }
          log_("info", "LLM 建议已接收: " + detail +
                           " tick=" + std::to_string(tick) +
                           "（是否采纳由策略层判定）");
        }
      } else {
        if (log_) {
          log_("warning", "LLM 输出未通过校验（tick=" + std::to_string(tick) +
                              "），本次按本地规则决策");
        }
      }
      return;
    }
    if (result.status == 401) {
      auth_failed_.store(true);
      record_error("令牌鉴权失败（401），请核对本场席位令牌");
      return;
    }
    if (result.status == 429) {
      const std::string code = error_code_of(result.body);
      if (code == "quota_exceeded") {
        quota_exhausted_.store(true);
        record_error("LLM 额度已用尽（429 quota_exceeded），此后仅本地决策");
        return;
      }
      if (attempt < 2) {
        if (log_) {
          log_("warning", "LLM 限流（429），第 " + std::to_string(attempt + 1) +
                              " 次退避重试");
        }
        if (wait_for(2000L * (attempt + 1))) {
          return;
        }
        continue;
      }
      record_error("LLM 限流重试仍失败");
      return;
    }
    if (result.status == 502 || result.status == 504) {
      record_error("上游不可用（" + std::to_string(result.status) + "）");
      return;
    }
    if (result.status == 413) {
      record_error("提示词超过 2 MiB（413）");
      return;
    }
    record_error("网关返回 " + std::to_string(result.status));
    return;
  }
}

LLMClient::PostResult LLMClient::post(const std::string& prompt) {
  PostResult result;
  json payload;
  payload["messages"] = json::array(
      {json{{"role", "system"}, {"content", system_prompt()}},
       json{{"role", "user"}, {"content", prompt}}});
  payload["stream"] = false;
  payload["max_tokens"] = max_tokens_;
  if (model_.has_value()) {
    payload["model"] = *model_;
  }
  const std::string data = payload.dump();
  if (data.size() > kMaxRequestBytes) {
    result.error = "提示词超过 2 MiB";
    return result;
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.error = "curl_easy_init 失败";
    return result;
  }
  const std::string url = gateway_ + "/v1/chat/completions";
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers,
                              ("Authorization: Bearer " + token_).c_str());
  std::string body;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_seconds_ * 1000);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // 线程内使用必须禁用信号
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);  // 协议：不跟随重定向

  const CURLcode code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    result.error = curl_easy_strerror(code);
  } else {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    result.status = status;
    result.ok = true;
    result.body = body;
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return result;
}

std::string LLMClient::check_health(const std::string& gateway,
                                    long timeout_seconds) {
  const std::string base = normalize_gateway(gateway);
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return "不可达: curl 初始化失败";
  }
  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, (base + "/healthz").c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_seconds * 1000);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  const CURLcode code = curl_easy_perform(curl);
  std::string description;
  if (code != CURLE_OK) {
    description = "不可达: " + std::string(curl_easy_strerror(code));
  } else {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (body.size() > 200) {
      body.resize(200);
    }
    description = "HTTP " + std::to_string(status) + " " + body;
  }
  curl_easy_cleanup(curl);
  return description;
}

}  // namespace rmtc
