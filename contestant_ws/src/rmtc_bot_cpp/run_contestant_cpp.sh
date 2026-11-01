#!/usr/bin/env bash
# RMTC 参赛程序启动脚本（C++ 版）。
# 用法: ./run_contestant_cpp.sh <席位 A|B|C|D> [队名] [--decision-config 路径]
# 例（公开测试端）:
#   export ROS_DOMAIN_ID=27 ROS_LOCALHOST_ONLY=0
#   ./run_contestant_cpp.sh A '星火队'
# 上场时按主办方通知设置席位、ROS_DOMAIN_ID、RMTC_AI_GATEWAY、RMTC_TEAM_TOKEN。
set -euo pipefail

SEAT="${1:-${RMTC_TEAM:-}}"
NAME="${2:-${RMTC_TEAM_NAME:-rmtc_bot}}"
[ -n "${SEAT}" ] || { echo "用法: ./run_contestant_cpp.sh <席位 A|B|C|D> [队名]" >&2; exit 2; }
[ "$#" -ge 1 ] && shift
[ "$#" -ge 1 ] && shift

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS_INSTALL="$(cd "${SCRIPT_DIR}/../../install" 2>/dev/null && pwd || true)"
set +u
if [ -z "${ROS_DISTRO:-}" ]; then
  source /opt/ros/humble/setup.bash
fi
if [ -n "${WS_INSTALL:-}" ] && [ -f "${WS_INSTALL}/setup.bash" ]; then
  source "${WS_INSTALL}/setup.bash"
fi
set -u
export ROS_LOCALHOST_ONLY="${ROS_LOCALHOST_ONLY:-0}"
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-27}"
# 公开测试端默认网关；正式比赛按主办方通知替换。RMTC_AI_URL 为别名，可写 base 或完整端点。
export RMTC_AI_GATEWAY="${RMTC_AI_GATEWAY:-${RMTC_AI_URL:-http://127.0.0.1:8080}}"
export RMTC_TEAM_TOKEN="${RMTC_TEAM_TOKEN:-practice-${SEAT}}"

echo "席位=${SEAT} 队名=${NAME} ROS_DOMAIN_ID=${ROS_DOMAIN_ID}"
echo "AI 网关=${RMTC_AI_GATEWAY}（令牌不显示）"

exec ros2 run rmtc_bot_cpp rmtc_bot_cpp "${SEAT}" "${NAME}" "$@"
