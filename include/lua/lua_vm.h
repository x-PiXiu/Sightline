// lua_vm.h —— Lua 5.4 配置虚拟机：沙箱 + 类型化读取 + 原子热更（服务端 01 文档 D1）
//
// 职责边界：只做"读配置表"。玩法脚本化（档③）为演进位，不在本类。
// 线程约定：loadFile/hotReload 与读取均发生在主 EventLoop 线程（reload 命令
//           由控制台线程经 queueInLoop 回投主线程执行），内部互斥锁兜底。

#pragma once

#include <string>
#include <memory>
#include <mutex>

struct lua_State;

namespace sightline {

class LuaVM
{
public:
    /** 新建 state → 沙箱化 → 执行脚本 → 校验返回表 → 原子替换。
     *  失败（文件缺失/语法错误/未返回表）保留旧状态并返回 false。 */
    bool loadFile(const std::string& path);

    /** 热更 = loadFile（校验后原子交换；失败保留旧配置，供"reload"命令调用） */
    bool hotReload(const std::string& path) { return loadFile(path); }

    // 类型化读取：从全局 CONFIG 表读取，缺键/类型不符返回 def
    std::string getString(const char* table, const char* key, const std::string& def) const;
    int         getInt(const char* table, const char* key, int def) const;
    double      getNumber(const char* table, const char* key, double def) const;

    bool loaded() const;

private:
    void sandbox(lua_State* L) const;                // 摘除 os/io/require 危险入口

    std::shared_ptr<lua_State> L_;                   // shared_ptr 定制删除器 lua_close，热替换原子
    mutable std::mutex mtx_;
};

} // namespace sightline
