#!/usr/bin/env bash
# db_up.sh —— ep08 步骤 5：三库健康检查 + 幂等 DDL（§5.7 full 模式启动前置）
# 用法：./scripts/db_up.sh [--wait]
# 本机形态（2026-11-07 实勘）：MySQL/Redis/MongoDB 均为面板托管的既有服务——
#   MySQL  127.0.0.1:3306（root@'%' 面板账号；眼线用专用账号 sightline/sightline）
#   Redis  127.0.0.1:6379（requirepass——密码只走 SIGHTLINE_REDIS_URI 第三段，绝不入库）
#   Mongo  127.0.0.1:27017（8.0.32·无密码）
# 本脚本只做：健康探测 → 幂等建库建表 → 打印 full 模式环境变量模板。
set -uo pipefail
cd "$(dirname "$0")/.."

WAIT=0
[ "${1:-}" = "--wait" ] && WAIT=1
MYSQL_PORT=3306

log()  { echo "· $*"; }
fail() { echo "✗ $*"; }

# ---- MySQL（探测 + 幂等 DDL）----
mysql_ok() { mysql -h127.0.0.1 -P"$MYSQL_PORT" -usightline -psightline sightline \
                 -e "SELECT 1" >/dev/null 2>&1; }
if mysql_ok; then
    log "MySQL(3306) ✓（sightline 账号可用）"
else
    log "sightline 账号/库未就绪——用面板 root 执行 DDL（密码走你本地输入，不回显）"
    read -r -p "  MySQL root@% 密码（面板账号，回车跳过）: " BTROOT || BTROOT=""
    if [ -n "$BTROOT" ]; then
        mysql -h172.23.34.108 -P"$MYSQL_PORT" -uroot -p"$BTROOT" <<'SQL' \
            && log "DDL 已执行" || fail "DDL 失败（密码错或无权限）"
CREATE DATABASE IF NOT EXISTS sightline DEFAULT CHARSET utf8mb4;
CREATE USER IF NOT EXISTS 'sightline'@'localhost' IDENTIFIED BY 'sightline';
CREATE USER IF NOT EXISTS 'sightline'@'127.0.0.1' IDENTIFIED BY 'sightline';
CREATE USER IF NOT EXISTS 'sightline'@'%' IDENTIFIED BY 'sightline';
GRANT ALL ON sightline.* TO 'sightline'@'localhost';
GRANT ALL ON sightline.* TO 'sightline'@'127.0.0.1';
GRANT ALL ON sightline.* TO 'sightline'@'%';
FLUSH PRIVILEGES;
SQL
        mysql -h127.0.0.1 -P"$MYSQL_PORT" -usightline -psightline sightline < assets/db/init.sql \
            && log "init.sql 已执行"
    else
        fail "跳过 DDL——full 模式横幅将显 MySQL ✗"
    fi
fi

# ---- Redis（密码走 SIGHTLINE_REDIS_URI 第三段——本脚本不持有密码）----
REDIS_URI="${SIGHTLINE_REDIS_URI:-127.0.0.1:6379}"
RPASS=""
case "$REDIS_URI" in *:*:*) RPASS="${REDIS_URI##*:}";; esac
if [ -n "$RPASS" ]; then
    redis-cli -a "$RPASS" ping 2>/dev/null | grep -q PONG && log "Redis(6379) ✓（已认证）" \
        || fail "Redis 认证失败——检查 SIGHTLINE_REDIS_URI 第三段"
else
    redis-cli ping 2>/dev/null | grep -q PONG && log "Redis(6379) ✓" \
        || fail "Redis 需要密码——export SIGHTLINE_REDIS_URI=127.0.0.1:6379:<密码>"
fi

# ---- MongoDB（面板托管 27017·无密码）----
if timeout 3 bash -c 'echo > /dev/tcp/127.0.0.1/27017' 2>/dev/null; then
    log "MongoDB(27017) ✓"
    mongosh sightline < assets/db/init.js >/dev/null 2>&1 && log "init.js 幂等执行 ok"
else
    fail "MongoDB(27017) 未监听——面板启一下；战报 sink 将显 ✗（best-effort·不拦 full）"
fi

echo ""
echo "full 模式启动（把密码填进你自己的 shell 环境，不写入仓库）："
echo "  export SIGHTLINE_REDIS_URI=127.0.0.1:6379:<你的Redis密码>"
echo "  ./sightline_server --db-profile=full"
echo "lite（默认·零前置）：./sightline_server"
