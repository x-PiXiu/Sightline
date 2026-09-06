#!/usr/bin/env bash
# 依赖方向守护：三条红线，任何一条被破坏即退出码 1（可挂 CI / pre-commit）
# （自 f487a97 重构前版本恢复；脚本移入 scripts/build/ 后定位根目录需多退一级）
# 红线出处：《架构设计文档》第四节"可守护"承诺
set -e
cd "$(dirname "$0")/../.."
fail=0

echo "[1/3] 实体层净空：domain 不得 include logger/net/任何外部世界"
if grep -rn '#include "' include/domain/ | grep -v '#include "domain/' | grep -v '#include <'; then
    echo "  ✗ 违规（见上）"; fail=1
else
    echo "  ✓ 干净"
fi

echo "[2/3] 用例层净空：application 不得 include net/adapters/logger"
if grep -rn '#include "net/\|#include "adapters/\|#include "logger/' include/application/ ; then
    echo "  ✗ 违规（见上）"; fail=1
else
    echo "  ✓ 干净"
fi

echo "[3/3] 网络库通用：net 不得 include 任何 game 头"
if grep -rn '#include "domain/\|#include "application/\|#include "adapters/' include/net/ src/net/ ; then
    echo "  ✗ 违规（见上）"; fail=1
else
    echo "  ✓ 干净"
fi

exit $fail
