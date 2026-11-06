#!/usr/bin/env bash
# db_smoke.sh —— ep08 步骤 5：三库冒烟（起库→集成测试→可选杀库降级演示）
# 用法：./scripts/db_smoke.sh [--kill-demo]
#   --kill-demo：跑完后 kill mongod（若在跑）演示 §5.5 降级（对局零中断靠人肉复验/录屏）
set -uo pipefail
cd "$(dirname "$0")/.."

echo "=== 1) 起库 + 建表 ==="
./scripts/db_up.sh --wait || { echo "✗ 库未就绪"; exit 1; }

echo ""
echo "=== 2) 真库集成冒烟（合成事件流→MySQL/Redis 断言）==="
export SIGHTLINE_DB_SMOKE=1
export SIGHTLINE_MYSQL_DSN="${SIGHTLINE_MYSQL_DSN:-127.0.0.1:3306:sightline:sightline:sightline}"
export SIGHTLINE_REDIS_URI="${SIGHTLINE_REDIS_URI:-127.0.0.1:6379}"
BIN=/tmp/test_db_integration
if [ ! -x "$BIN" ]; then
    echo "（$BIN 不存在——先 ./build.sh all 编译）"; exit 1
fi
"$BIN" || { echo "✗ 集成冒烟失败"; exit 1; }

echo ""
echo "=== 3) 降级演示（可选）==="
if [ "${1:-}" = "--kill-demo" ]; then
    if pgrep mongod >/dev/null; then
        pkill -9 mongod && echo "mongod 已杀——下次 full 写战报走降级（丢弃计数+告警·对局照跑）"
    else
        echo "mongod 未在跑（本机无 mongod=降级态恒在·同语义）"
    fi
fi

echo ""
echo "✓ db_smoke 完成——full 模式可启动（见 db_up.sh 尾部环境变量）"
