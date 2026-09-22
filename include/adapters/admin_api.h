// admin_api.h —— GM 管理 API 路由（服务端 03 文档）：HTTP 请求 → application 用例
//
// 线程模型（本文件的全部意义）：
//   HTTP 线程不许直接读 Session/Room 状态——游戏逻辑单线程跑在主 EventLoop，
//   裸读会竞态。做法：promise/future 把"取数+操作"投进主 loop 执行，
//   HTTP 线程原地等结果（2s 超时兜底主 loop 卡死）。零锁，逻辑零改动。
//
// 依赖方向：adapters → application/domain（整洁架构外环向内，合规）。
// JSON 序列化放本层（表现细节），application 只吐结构体。

#pragma once

#include <chrono>
#include <cstdlib>
#include <future>
#include <fstream>
#include <sstream>
#include <string>
#include <regex>
#include <functional>
#include "adapters/admin_http_server.h"
#include "adapters/game_server.h"
#include "application/session_service.h"
#include "application/room_service.h"

namespace sightline::adapters {

class AdminApi
{
public:
    /** postToMain：把闭包投递到主 EventLoop（main 注入 loop.queueInLoop） */
    using PostToMain = std::function<void(std::function<void()>)>;
    /** reloadConfig：config.lua 热载（main 注入，与控制台 reload 同一实现） */
    using ReloadFn = std::function<void()>;
    /** configGetter：当前生效的可调参数快照（key 有序，值保证为数字字面量） */
    using ConfigGetter = std::function<std::vector<std::pair<std::string, std::string>>()>;
    /** configSetter：写回 config.lua + 立即热载；返回空串 = 成功，否则为错误信息。
     *  白名单校验归 main（组合根知道哪些键可热调），本层只做 JSON↔键值对翻译 */
    using ConfigSetter = std::function<std::string(const std::vector<std::pair<std::string, std::string>>&)>;;

    AdminApi(app::SessionService& sessions, app::RoomService& rooms,
             GameServer& server, PostToMain post_to_main,
             std::string token, ReloadFn reload_config,
             ConfigGetter get_config = {}, ConfigSetter set_config = {},
             std::string panel_file = {})
        : sessions_(sessions), rooms_(rooms), server_(server),
          post_to_main_(std::move(post_to_main)), token_(std::move(token)),
          reload_config_(std::move(reload_config)),
          get_config_(std::move(get_config)), set_config_(std::move(set_config)),
          panel_file_(std::move(panel_file)),
          started_at_(std::chrono::steady_clock::now()) {}

    HttpResponse handle(const HttpRequest& req)
    {
        // 面板页本身不走 token（token 由页面输入后经 fetch 头携带，存 localStorage）
        if (req.path == "/" || req.path == "/admin") return servePanel();
        if (req.path.rfind("/api/", 0) != 0)
            return {404, "application/json", "{\"error\":\"not found\"}"};

        // ---- 鉴权：Bearer token（03 文档缺口补丁——裸奔的 Admin API 等于把踢人权交给局域网）----
        if (token_.empty() || req.authorization != "Bearer " + token_)
            return {401, "application/json", "{\"error\":\"unauthorized\"}"};

        // ---- 跳主 loop 执行路由（读状态/操作的唯一通道）----
        std::promise<HttpResponse> prom;
        auto fut = prom.get_future();
        post_to_main_([&prom, &req, this] {
            HttpResponse result;
            try { result = routeLocked(req); }
            catch (const std::exception& e) { result = {500, "application/json", err(e.what())}; }
            catch (...) { result = {500, "application/json", err("internal")}; }
            prom.set_value(std::move(result));
        });
        if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
            return {500, "application/json", "{\"error\":\"main loop timeout\"}"};
        try { return fut.get(); }
        catch (const std::exception& e) { return {500, "application/json", err(e.what())}; }
    }

private:
    // ---- 路由表：主 loop 线程执行，可安全读服务状态 ----
    HttpResponse routeLocked(const HttpRequest& req)
    {
        const bool is_get  = req.method == "GET";
        const bool is_post = req.method == "POST";

        if (is_get && req.path == "/api/stats")      return {200, "application/json", statsJson()};
        if (is_get && req.path == "/api/players")    return {200, "application/json", playersJson()};
        if (is_get && req.path == "/api/rooms")      return {200, "application/json", roomsJson()};
        if (is_get && req.path == "/api/top-kills")  return {200, "application/json", topKillsJson()};
        if (is_get && req.path == "/api/config")     return {200, "application/json", configJson()};
        if (is_post && req.path == "/api/kick")      return {200, "application/json", kick(req.body)};
        if (is_post && req.path == "/api/config")    return configSet(req.body);
        if (is_post && req.path == "/api/reload-config")
        {
            if (!reload_config_) return {500, "application/json", err("reload not wired")};
            reload_config_();
            return {200, "application/json", "{\"ok\":true}"};
        }
        return {404, "application/json", err("unknown route")};
    }

    // ---- GET /api/config：当前生效的可调参数 ----
    std::string configJson()
    {
        if (!get_config_) return err("config getter not wired");
        std::ostringstream o;
        o << "{\"config\":{";
        bool first = true;
        for (const auto& [k, v] : get_config_())
        {
            if (!first) o << ",";
            first = false;
            o << jstr(k) << ":" << v;          // v 为数字字面量（main 侧保证）
        }
        o << "}}";
        return o.str();
    }

    // ---- POST /api/config：body {"updates":{"game.win_kills":5,...}} → 写回 + 热载 ----
    HttpResponse configSet(const std::string& body)
    {
        if (!set_config_) return {500, "application/json", err("config setter not wired")};
        std::vector<std::pair<std::string, std::string>> updates;
        static const std::regex re("\"([^\"]+)\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
             it != std::sregex_iterator(); ++it)
            updates.emplace_back((*it)[1].str(), (*it)[2].str());
        if (updates.empty())
            return {400, "application/json", err("no numeric updates found (body: {\"updates\":{...}})")};
        if (const std::string e = set_config_(updates); !e.empty())
            return {400, "application/json", err(e)};
        return {200, "application/json", "{\"ok\":true}"};
    }

    // ---- GET /api/stats：连接/房间/流量/运行时长 ----
    std::string statsJson()
    {
        const auto up = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started_at_).count();
        std::ostringstream o;
        o << "{\"connections\":" << server_.connectionCount()
          << ",\"rooms\":" << rooms_.roomCount()
          << ",\"msgs_in\":" << server_.msgsIn()
          << ",\"msgs_out\":" << server_.msgsOut()
          << ",\"bytes_in\":" << server_.bytesIn()
          << ",\"bytes_out\":" << server_.bytesOut()
          << ",\"uptime_sec\":" << up << "}";
        return o.str();
    }

    // ---- GET /api/players：会话列表 ∪ 所在房间（两服务在主 loop 里串行读，免锁合并）----
    std::string playersJson()
    {
        std::ostringstream o;
        o << "{\"players\":[";
        bool first = true;
        for (const auto& s : sessions_.listSessions())
        {
            if (!first) o << ",";
            first = false;
            o << "{\"player_id\":" << s.pid
              << ",\"name\":" << jstr(s.name)
              << ",\"account_id\":" << s.account_id
              << ",\"guest\":" << (s.guest ? "true" : "false")
              << ",\"room_id\":" << rooms_.roomIdOf(s.pid)
              << ",\"online_sec\":" << s.online_sec << "}";
        }
        o << "]}";
        return o.str();
    }

    // ---- GET /api/rooms：房间列表（状态 + 已进行时长）----
    std::string roomsJson()
    {
        std::ostringstream o;
        o << "{\"rooms\":[";
        bool first = true;
        for (const auto& r : rooms_.listRooms())
        {
            if (!first) o << ",";
            first = false;
            o << "{\"room_id\":" << r.room_id
              << ",\"state\":" << static_cast<int>(r.state)
              << ",\"cur_players\":" << r.cur_players
              << ",\"max_players\":" << r.max_players
              << ",\"elapsed_sec\":" << r.elapsed_sec << "}";
        }
        o << "]}";
        return o.str();
    }

    // ---- GET /api/top-kills：Redis ZSET TopN ----
    // 注：同步 Redis 调用在主 loop 执行——管理查询低频 + 内网 RTT 亚毫秒，可接受；
    // 高频查询应改走存储线程 + 回投（03 文档 P1 演进项）。
    std::string topKillsJson()
    {
        std::ostringstream o;
        o << "{\"top\":[";
        bool first = true;
        for (const auto& [account_id, kills] : rooms_.topKills(10))
        {
            if (!first) o << ",";
            first = false;
            o << "{\"account_id\":" << account_id << ",\"kills\":" << kills << "}";
        }
        o << "]}";
        return o.str();
    }

    // ---- POST /api/kick：body {"player_id":N}；语义 = 心跳超时同款（通告+断开→正常清理链）----
    std::string kick(const std::string& body)
    {
        const size_t key = body.find("\"player_id\"");
        if (key == std::string::npos) return err("player_id required");
        const size_t colon = body.find(':', key);
        const app::PlayerId pid = std::strtoull(body.c_str() + colon + 1, nullptr, 10);
        if (!sessions_.has(pid)) return err("player not online");
        sessions_.kick(pid, /*reason*/2);   // 1=心跳超时 2=GM 踢出
        return "{\"ok\":true}";
    }

    // ---- 面板页：GET / 返回静态 HTML（HTTP 线程读盘，不占游戏 loop；路径由 main 解析注入）----
    HttpResponse servePanel()
    {
        const std::string path = panel_file_.empty() ? "www/admin/index.html" : panel_file_;
        std::ifstream f(path, std::ios::binary);
        if (!f) return {404, "text/plain", "panel file not found: " + path};
        std::ostringstream ss; ss << f.rdbuf();
        return {200, "text/html; charset=utf-8", ss.str()};
    }

    // ---- JSON 小工具（管理面板够用的转义；接入 nlohmann 属 P1 演进）----
    static std::string jstr(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 0x20) { /* 控制字符丢弃 */ }
            else out += c;
        }
        return out + "\"";
    }
    static std::string err(const std::string& msg) { return "{\"error\":" + jstr(msg) + "}"; }

    app::SessionService& sessions_;
    app::RoomService&    rooms_;
    GameServer&          server_;
    PostToMain           post_to_main_;
    std::string          token_;
    ReloadFn             reload_config_;
    ConfigGetter         get_config_;
    ConfigSetter         set_config_;
    std::string          panel_file_;
    std::chrono::steady_clock::time_point started_at_;
};

} // namespace sightline::adapters
