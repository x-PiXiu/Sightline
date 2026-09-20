// test_lua.cpp —— LuaVM 单测：加载/读取/缺键默认值/热更原子性

#include "lua/lua_vm.h"
#include <cassert>
#include <cstdio>
#include <fstream>

static void writeFile(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary);
    f << content;
}

int main()
{
    sightline::LuaVM vm;

    // 1) 合法配置：return 一张表
    writeFile("test_cfg_ok.lua", "return { network = { port = 9527 }, game = { win_kills = 5, name = \"sightline\" } }");
    if (!vm.loadFile("test_cfg_ok.lua")) { std::puts("[FAIL] 合法配置加载失败"); return 1; }

    // 2) 类型化读取（CONFIG.<table>.<key> 两级导航）
    if (vm.getInt("network", "port", 0) != 9527)  { std::puts("[FAIL] getInt network.port"); return 1; }
    if (vm.getInt("game", "win_kills", 0) != 5)   { std::puts("[FAIL] getInt game.win_kills"); return 1; }
    if (vm.getString("game", "name", "") != "sightline") { std::puts("[FAIL] getString game.name"); return 1; }

    // 3) 缺键 → 默认值
    if (vm.getInt("game", "not_exist", 77) != 77) { std::puts("[FAIL] 缺键未取默认值"); return 1; }

    // 4) 语法错误 → 加载失败且保留旧配置（热更原子性）
    writeFile("test_cfg_bad.lua", "return { syntax_error === }");
    if (vm.hotReload("test_cfg_bad.lua")) { std::puts("[FAIL] 坏配置竟加载成功"); return 1; }
    if (vm.getInt("network", "port", 0) != 9527)  { std::puts("[FAIL] 失败热更后旧配置丢失"); return 1; }

    // 5) 未返回表 → 拒绝
    writeFile("test_cfg_notable.lua", "print(\"hi\")");
    if (vm.hotReload("test_cfg_notable.lua")) { std::puts("[FAIL] 未返回表竟加载成功"); return 1; }

    std::remove("test_cfg_ok.lua");
    std::remove("test_cfg_bad.lua");
    std::remove("test_cfg_notable.lua");

    std::puts("[PASS] LuaVM 全部用例通过");
    return 0;
}
