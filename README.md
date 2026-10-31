# RMTC 机甲大师图灵对决赛 · 参赛程序 `rmtc_bot_cpp`

参赛程序运行于 ROS 2 Humble，订阅本席位感知与裁判公开公告，每个 Tick 输出一个合法动作；第一阶段完成自主采集，第二阶段进行资源对抗、任务与随机事件处理。随机事件公告以原文交给官方 AI 网关的大语言模型理解，逐 Tick 动作始终由本地策略执行。

---

## 1. 目录结构、运行环境与依赖

### 1.1 目录结构

```text
contestant_ws/
└── src/
    └── rmtc_bot_cpp/
        ├── CMakeLists.txt
        ├── package.xml
        ├── run_contestant_cpp.sh          # 启动脚本
        ├── config/
        │   └── decision_config.json       # 决策类型与策略参数
        ├── include/
        │   ├── nlohmann/json.hpp          # 第三方单头文件
        │   └── rmtc_bot_cpp/
        │       ├── types.hpp
        │       ├── protocol.hpp
        │       ├── world.hpp
        │       ├── planner.hpp
        │       ├── llm_client.hpp
        │       └── strategy.hpp
        └── src/
            ├── main.cpp
            ├── protocol.cpp
            ├── world.cpp
            ├── planner.cpp
            ├── llm_client.cpp
            └── strategy.cpp
```

`build/`、`install/`、`log/` 为构建和运行产物，不纳入版本管理。

### 1.2 运行环境与依赖

- Ubuntu 22.04 + ROS 2 Humble
- C++17 编译器
- `rclcpp`、`std_msgs`、`ament_index_cpp`
- `libcurl4-openssl-dev`
- `nlohmann/json` 已随源码提供（`include/nlohmann/json.hpp`），无需单独安装

```bash
sudo apt update
sudo apt install -y ros-humble-rclcpp ros-humble-std-msgs \
  ros-humble-ament-index-cpp libcurl4-openssl-dev
```

---

## 2. 构建方法、启动目录与完整启动命令

### 2.1 构建

以下命令均在仓库根目录（本 README 所在目录，即 `contestant_ws/`）执行：

```bash
cd ~/contestant_ws
source /opt/ros/humble/setup.bash
colcon build --symlink-install --packages-select rmtc_bot_cpp
source install/setup.bash
ros2 pkg prefix rmtc_bot_cpp
```

`ros2 pkg prefix` 应指向当前工作空间的 `install/rmtc_bot_cpp`。推荐使用 `--symlink-install`，便于修改配置后直接生效。

### 2.2 启动目录

启动前必须先 `source` ROS 环境和本工作空间的 `install/setup.bash`，并在仓库根目录执行启动命令，保证 `src/rmtc_bot_cpp/run_contestant_cpp.sh` 与 `install/` 路径正确。

```bash
cd ~/contestant_ws
source /opt/ros/humble/setup.bash
source install/setup.bash
```

### 2.3 完整启动命令

**方式一：使用启动脚本**

```bash
cd ~/contestant_ws
source /opt/ros/humble/setup.bash
source install/setup.bash

export ROS_DOMAIN_ID=<本场域编号>
export ROS_LOCALHOST_ONLY=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export RMTC_AI_GATEWAY=http://<本场网关地址>:<端口>
export RMTC_TEAM_TOKEN='<本场席位令牌>'

./src/rmtc_bot_cpp/run_contestant_cpp.sh A '路边队'
```

**方式二：直接使用 `ros2 run`**

```bash
cd ~/contestant_ws
source /opt/ros/humble/setup.bash
source install/setup.bash

ros2 run rmtc_bot_cpp rmtc_bot_cpp A '路边队' \
  --decision-config "$PWD/src/rmtc_bot_cpp/config/decision_config.json"
```

## 3. 第一阶段、第二阶段的启动与切换

两个阶段使用**完全相同的参赛端启动命令**，不需要额外参数。阶段由裁判端切换，程序根据感知内容自动识别：

| 阶段 | 参赛端启动 | 阶段特征与程序行为 |
| --- | --- | --- |
| 第一阶段 | 席位 `A`，使用上面的完整启动命令 | 只有本车、无任务、无敌人；只输出 `move` / `wait`，采集资源并探索 |
| 第二阶段 | 席位按本场分配（A/B/C/D），命令不变 | 出现任务、敌方机器人或多席位名录时自动进入；启用攻击、防御、目标竞争与 LLM 建议 |

切换与重置行为：

- `session_id` 变化时，程序自动清空上一场的地图记忆、动作缓存、事件计划、LLM 目标与幻术窗口，不需要重启节点。
- 同一进程可连续参加多场；如裁判要求重启程序，重新执行上面的启动命令即可。
- 决策类型通过 `config/decision_config.json` 的 `decision_type` 选择：`collect`（采集优先）、`combat`（对抗优先）、`balanced`（默认平衡）。程序每秒重读配置。
- 按赛事规则，代码封印后现场只允许按裁判登记流程修改 `decision_type`，不得修改源码、算法、Prompt 或其他策略参数。

---

## 4. 席位、ROS 域、AI 网关地址及令牌配置

### 4.1 配置方式

| 配置项 | 设置方式 | 说明 |
| --- | --- | --- |
| 席位 | 第一个位置参数，或 `RMTC_TEAM` | 必填，`A` / `B` / `C` / `D`，须与裁判分配一致 |
| 队名 | 第二个位置参数，或 `RMTC_TEAM_NAME` | 默认 `rmtc_bot`；1–96 个 UTF-8 字节，不含换行 |
| ROS 域 | `ROS_DOMAIN_ID` | 必须与裁判端一致 |
| 仅本机通信 | `ROS_LOCALHOST_ONLY` | 必须为 `0` |
| DDS 实现 | `RMW_IMPLEMENTATION` | 与裁判端一致；公开测试端默认 `rmw_fastrtps_cpp` |
| AI 网关 | `RMTC_AI_GATEWAY` | 只填 base URL，例如 `http://192.168.1.100:8000`；**不要**带 `/v1/chat/completions` |
| 席位令牌 | `RMTC_TEAM_TOKEN` | 本场令牌，通过 `Authorization: Bearer` 使用，禁止写入代码、配置、日志或截图 |
| 模型名 | `RMTC_AI_MODEL` | 可选；不设置时使用网关默认模型 |
| 最大输出 | `RMTC_AI_MAX_TOKENS` | 默认 `600` |
| 请求超时 | `RMTC_AI_TIMEOUT` | 默认 `30` 秒 |
| 决策配置 | `--decision-config <路径>` 或 `RMTC_DECISION_CONFIG` | 默认读取安装目录下的 `config/decision_config.json` |

### 4.2 配置示例

```bash
export ROS_DOMAIN_ID=27
export ROS_LOCALHOST_ONLY=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export RMTC_AI_GATEWAY=http://192.168.1.100:8000
export RMTC_TEAM_TOKEN='<本场席位令牌>'

bash ./src/rmtc_bot_cpp/run_contestant_cpp.sh A '路边队'
```

启动脚本内置了公开测试端的默认值：

- `ROS_DOMAIN_ID=27`
- `RMTC_AI_GATEWAY=http://127.0.0.1:8080`
- `RMTC_TEAM_TOKEN=practice-<席位>`

这些默认值**只适用于练习**。正式比赛必须按裁判通知覆盖 ROS 域、网关地址和席位令牌，并确认席位使用大写字母。

---

## 5. 程序主要模块与运行逻辑

### 5.1 主要模块

| 模块 | 职责 |
| --- | --- |
| `main.cpp` | ROS 接入层：感知/公告/任务/名录订阅、动作与注册发布、场次切换、同 Tick 重发、监控日志、LLM 触发 |
| `protocol.cpp` | 协议层：感知 JSON 解析、9×9 相对位置解析、动作与注册消息构造 |
| `world.cpp` | 世界模型：单场次状态、视野合并、资源/障碍/敌人记忆、公告去重、幻术回执学习 |
| `planner.cpp` | 规划层：Dijkstra 最短路、危险格代价、目标评分、前沿探索 |
| `strategy.cpp` | 决策层：硬约束、即时战术、目标竞争、事件计划执行、LLM 建议融合 |
| `llm_client.cpp` | LLM 客户端：提示词构造、HTTP 调用、响应校验、后台线程与失败降级 |

`main.cpp` 是包内唯一直接依赖 `rclcpp` 的模块，其余模块为纯逻辑库。

### 5.2 每 Tick 运行顺序

1. 解析感知；字段非法则丢弃该帧并记录日志，进程不退出。
2. `session_id` 变化时清空上一场状态并重新初始化。
3. 丢弃早于当前 Tick 的同场次感知；同一 Tick 重发时原样重发缓存动作，不重复推进策略。
4. 更新世界模型：本车位置直接采用 `self.position`，视野实体按 9×9 相对位置换算为绝对坐标。
5. 执行决策：硬约束 → 事件计划 → 即时战术 → 目标竞争 → 探索 → 等待。
6. 发布前检查存活状态、阶段限制、出生保护和方向字段，回填当前感知的 `tick` 与 `session_id`。
7. 发布动作；第二阶段按条件在后台触发 LLM 调用。

### 5.3 决策逻辑

**第一阶段**：阵亡时只发 `wait` → 资源采集（按价值与路径长度竞争）→ 未知区域探索 → 无目标时 `wait`；只允许 `move` / `wait`。

**第二阶段**：

1. **硬约束**：无场次、阵亡时只发 `wait`；出生保护期内不攻击。
2. **事件计划**：执行 LLM 依据公告给出的 `wait` / `defend` / `move_to`，按绝对 Tick 过期；遇即时火力威胁时让位本地生存动作。
3. **即时战术**：射程内攻击、出生保护、残血防御、逃离或防御敌方火力线。
4. **目标竞争**：资源、`reach` 任务、LLM 目标与追击敌人统一按“权重 / 路径长度”评分；残血时限制追击半径。
5. **探索**：无目标时前往未知前沿，并保持短时方向承诺，避免逐 Tick 抖动。
6. **等待**：无可用动作时输出 `wait`。

### 5.4 关键运行机制

- **发布前防线**：阵亡强制 `wait`；第一阶段禁止 `attack` / `defend`；保护期禁止攻击；`move` / `attack` 必须带合法方向。
- **场次隔离**：换场时清空地图记忆、事件计划、LLM 目标、幻术窗口和动作缓存。
- **同 Tick 重发**：暂停恢复或网络抖动导致的重复感知只重发缓存动作，不重复规划。
- **异步 LLM**：单后台线程、latest-wins 单槽队列，同一时刻最多一个在途请求；401 或额度耗尽后停止调用，其他错误退避重试或直接降级。
- **线程与定时器**：主线程单线程执行器处理 ROS 回调；注册消息每 5 秒重发，配置与感知超时每秒检查。
