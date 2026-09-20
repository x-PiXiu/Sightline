// lua_vm.cpp —— LuaVM 实现（Lua 5.4 C API）

#include "lua/lua_vm.h"
#include "logger/logger.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <utility>

namespace sightline {

namespace {
    // shared_ptr 定制删除器：析构时安全关闭 lua_State
    void closeLua(lua_State* L) { if (L) lua_close(L); }

    /** 摘除危险库/入口——配置脚本不需要 os/io/require（沙箱纪律：企业脚本系统标配） */
    void stripDangerousGlobals(lua_State* L)
    {
        lua_getglobal(L, "_G");
        for (const char* name : { "os", "io", "require", "dofile", "loadfile" })
        {
            lua_pushnil(L);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);   // 弹出 _G
    }

    /** 取 CONFIG.<table>.<key> 的 string（两级导航 + 栈平衡） */
    bool fieldString(lua_State* L, const char* table, const char* key, std::string& out)
    {
        lua_getglobal(L, "CONFIG");                     // [CONFIG]
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        lua_getfield(L, -1, table);                     // [CONFIG, 子表]
        if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }
        lua_getfield(L, -1, key);                       // [CONFIG, 子表, 值]
        const bool ok = lua_isstring(L, -1) != 0;
        if (ok) out = lua_tostring(L, -1);
        lua_pop(L, 2);
        return ok;
    }

    /** 取 CONFIG.<table>.<key> 的 number（两级导航 + 栈平衡） */
    bool fieldNumber(lua_State* L, const char* table, const char* key, double& out)
    {
        lua_getglobal(L, "CONFIG");                     // [CONFIG]
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        lua_getfield(L, -1, table);                     // [CONFIG, 子表]
        if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }
        lua_getfield(L, -1, key);                       // [CONFIG, 子表, 值]
        const bool ok = lua_isnumber(L, -1) != 0;
        if (ok) out = lua_tonumber(L, -1);
        lua_pop(L, 2);                                  // 弹值 + 子表，留 CONFIG
        return ok;
    }
}

bool LuaVM::loadFile(const std::string& path)
{
    lua_State* L = luaL_newstate();
    if (!L) return false;

    luaL_openlibs(L);        // 开全量标准库 → 随即沙箱摘除危险入口
    stripDangerousGlobals(L);

    if (luaL_dofile(L, path.c_str()) != LUA_OK)
    {
        const char* msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "unknown";
        LOG_ERROR(std::string("[LuaVM] 配置脚本执行失败: ") + path + " (" + msg + ")");
        lua_close(L);
        return false;
    }

    // 契约：脚本必须 return 一张表（即配置本体），否则视为非法配置
    if (!lua_istable(L, -1))
    {
        LOG_ERROR(std::string("[LuaVM] 配置脚本未返回表: ") + path);
        lua_close(L);
        return false;
    }
    lua_setglobal(L, "CONFIG");   // 返回表固化为全局 CONFIG，读取接口从这里取

    std::lock_guard<std::mutex> lk(mtx_);
    L_ = std::shared_ptr<lua_State>(L, closeLua);   // 原子替换（失败路径不会走到这）
    LOG_INFO(std::string("[LuaVM] 配置已加载: ") + path);
    return true;
}

std::string LuaVM::getString(const char* table, const char* key, const std::string& def) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!L_) return def;
    std::string out;
    if (fieldString(L_.get(), table, key, out)) return out;
    return def;
}

int LuaVM::getInt(const char* table, const char* key, int def) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!L_) return def;
    double v = 0;
    if (fieldNumber(L_.get(), table, key, v)) return static_cast<int>(v);
    return def;
}

double LuaVM::getNumber(const char* table, const char* key, double def) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!L_) return def;
    double v = def;
    fieldNumber(L_.get(), table, key, v);
    return v;
}

bool LuaVM::loaded() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return L_ != nullptr;
}

} // namespace sightline
