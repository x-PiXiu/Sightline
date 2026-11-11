// test_account.cpp —— ep09 步骤 5（刀 A）：账号域纯逻辑单测（ADR-22/G9/E7/E9 契约·零 IO）
// 断言面：①PBKDF2 往返（Hash→Verify 同 pass ✓/异 pass ✗）+存储串格式+盐随机（同明文异盐）
//         ②恒时比较语义（格式篡改拒）③合法性预检（account 正则/长度/pass 边界）
//         ④AccountService 流转（Register/Login/Query 投递+Reject 即时 emit+Unavailable）
//         ⑤E9 寻址字段在 DTO（fd 快照）⑥E7 语义（accountId 非 pid——DTO 结构验证）
#include "adapters/password_pbkdf2.h"
#include "application/account_service.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace sightline::application;
static int g_pass = 0;
#define CHECK(cond) do { if (cond) { ++g_pass; } else { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {
// fake store：记录投递（fd/account/pass）——验证 service→worker 异步边界
struct FakeStore final : IAccountStore {
    struct Req { int fd; std::string account, pass; std::uint32_t accountId = 0, limit = 0; };
    enum class Kind : std::uint8_t { Register, Login, Query };
    std::vector<std::pair<Kind, Req>> calls;
    void Register(int fd, const std::string& a, const std::string& p) override {
        calls.push_back({Kind::Register, {fd, a, p}});
    }
    void Login(int fd, const std::string& a, const std::string& p) override {
        calls.push_back({Kind::Login, {fd, a, p}});
    }
    void QueryStats(int fd, std::uint32_t aid, std::uint32_t limit) override {
        calls.push_back({Kind::Query, {fd, "", "", aid, limit}});
    }
};

// fake emit：收集预检失败回执（Reject 即时 emit 通道）
std::vector<AccountResult>* g_emit = nullptr;
} // namespace

int main() {
    adapters::Pbkdf2Hasher hasher;

    // ── 断言 1：PBKDF2 往返 ──
    {
        const std::string stored = hasher.Hash("hunter2!");
        CHECK(stored.rfind("pbkdf2-sha256$12000$", 0) == 0);   // 格式锁（iter=12000·P4）
        CHECK(stored.find('$', 19) != std::string::npos);      // 三段式（iter$salt$hash）
        CHECK(hasher.Verify("hunter2!", stored));
        CHECK(!hasher.Verify("hunter3!", stored));             // 异明文拒
        CHECK(!hasher.Verify("", stored));
    }

    // ── 断言 2：盐随机（同明文两次 Hash → 不同存储串）+篡改拒 ──
    {
        const auto a = hasher.Hash("same-pass");
        const auto b = hasher.Hash("same-pass");
        CHECK(a != b);                                         // 盐随机（防彩虹表）
        CHECK(hasher.Verify("same-pass", a) && hasher.Verify("same-pass", b));
        CHECK(!hasher.Verify("same-pass", a.substr(0, a.size() - 2)));   // 截断篡改拒
        CHECK(!hasher.Verify("same-pass", "garbage"));         // 格式拒
    }

    // ── 断言 3：合法性预检（契约 v4.9 规则）──
    {
        CHECK(AccountService::ValidAccount("abc_123"));
        CHECK(!AccountService::ValidAccount("a"));             // <2
        CHECK(!AccountService::ValidAccount(std::string(25, 'x')));   // >24
        CHECK(!AccountService::ValidAccount("bad-name"));      // '-' 非法字符
        CHECK(!AccountService::ValidAccount("中文名"));         // 非 ASCII
        CHECK(!AccountService::ValidPass("12345"));            // <6
        CHECK(AccountService::ValidPass(std::string(64, 'p')));
        CHECK(!AccountService::ValidPass(std::string(65, 'p')));
    }

    // ── 断言 4：AccountService 流转（投递/Reject emit/Unavailable）──
    {
        std::vector<AccountResult> emitted;
        FakeStore store;
        AccountService::Deps deps{&hasher, &store,
                                  [&](const AccountResult& r) { emitted.push_back(r); }};
        AccountService svc{deps};

        CHECK(svc.Register(801, "alice", "secret1"));          // 合法→投递
        CHECK(svc.Login(801, "alice", "secret1"));
        svc.QueryStats(801, 42, 10);                           // 登录态查询（accountId=42）
        CHECK(store.calls.size() == 3);
        CHECK(store.calls[0].first == FakeStore::Kind::Register &&
              store.calls[0].second.fd == 801 &&
              store.calls[0].second.account == "alice");
        CHECK(emitted.empty());                                // 合法路径零即时回执（等 worker）

        // 预检失败：即时 emit（Reject 同步通道）——不经 store
        CHECK(!svc.Register(802, "bad name", "secret1"));      // '-' 非法
        CHECK(!svc.Login(802, "x", "secret1"));                // account <2
        CHECK(!svc.QueryStats(802, 0, 10));                    // 未登录（accountId=0）
        CHECK(emitted.size() == 3);
        CHECK(emitted[0].reason == AuthReason::BadParams &&
              emitted[0].fd == 802 && !emitted[0].ok);
        CHECK(store.calls.size() == 3);                        // store 零新增（预检拦截）

        // limit 钳制：0/超 20 → 20
        svc.QueryStats(801, 42, 99);
        CHECK(store.calls.back().second.limit == 20);

        // Unavailable：store 未装
        AccountService::Deps empty{&hasher, nullptr, [&](const AccountResult& r) { emitted.push_back(r); }};
        AccountService svcDown{empty};
        CHECK(!svcDown.Login(803, "bob", "secret1"));
        CHECK(emitted.back().reason == AuthReason::Unavailable);
    }

    // ── 断言 5：E9 寻址字段 + E7 语义（DTO 结构验证）──
    {
        AccountResult r;
        r.fd = 801;
        r.accountId = 42;
        CHECK(r.fd == 801 && r.accountId == 42 && r.accountId != r.fd);   // accountId≠fd（pid 语义隔离）
        r.rows.push_back({7, 3, 1, 1700000000});               // 35 号行（14B 载荷源）
        CHECK(r.rows[0].matchId == 7 && r.rows[0].kills == 3);
        CHECK(r.reason == AuthReason::Ok);                     // 枚举值 0=契约锁
        CHECK(static_cast<std::uint8_t>(AuthReason::Unavailable) == 5);
    }

    std::printf("全部通过：%d 项断言 ✓（ep09 步骤 5 账号域纯逻辑·ADR-22/G9/E7/E9）\n", g_pass);
    return 0;
}
