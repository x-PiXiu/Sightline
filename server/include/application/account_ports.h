// account_ports.h —— ep09 步骤 5（刀 A）：账号域端口与 DTO（用例层所有·G9/ADR-16/22）
// 域注记（草案 §2 域事件地图）：账号域=第三域事件流——AccountResult 经 persistence 回流队列
//   →主线程→DeliverAuth（独立 Fanout 口，与对局域/配置域互不交叉·跨域无顺序保证）。
// ADR-16：注册/登录/查询**全异步**（请求-稍后回执模型）——主线程只做合法性预检（微秒级），
//   PBKDF2 计算与 MySQL/Redis IO 全在 persistence worker（主线程零阻塞重活·A1 纪律延伸）。
// ADR-22：密码哈希=安全业务规则（端口归用例层）+PBKDF2 实现=机制（适配器可换 argon2 等）。
// E7：accountId ≠ pid——身份层与在线层解耦（pid 仍由进大厅/建房路径分配·既有 4 号机制零改）。
// E9：回执寻址=发起时 fd 快照（fd 失效=丢+计数，重连重查——查询无状态幂等）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sightline::application {

// ===== 合法性规则（实体层语义·与 MSG_IDS v4.9 登记一致）=====
inline constexpr std::size_t kAccountMinLen = 2;
inline constexpr std::size_t kAccountMaxLen = 24;
inline constexpr std::size_t kPassMinLen = 6;
inline constexpr std::size_t kPassMaxLen = 64;

/** 33 号 reason 枚举（契约 v4.9 锁） */
enum class AuthReason : std::uint8_t {
    Ok = 0,          // 成功
    AlreadyExists = 1,   // 已存在（注册）
    NotFound = 2,        // 不存在（登录）
    WrongPass = 3,       // 密码错
    BadParams = 4,       // 参数非法
    Unavailable = 5,     // 服务不可用（队列满/库故障）
};

/** 密码哈希端口（ADR-22——安全规则归用例层，实现归适配器）。
 *  存储格式：pbkdf2-sha256$<iter>$<salt_b64>$<hash_b64>（t_account.pass_hash VARCHAR(128)）。
 *  verify 用恒时比较（CRYPTO_memcmp）——侧信道防御。 */
struct IPasswordHasher {
    virtual ~IPasswordHasher() = default;
    virtual std::string Hash(const std::string& plain) const = 0;             // 明文→存储串（盐随机）
    virtual bool Verify(const std::string& plain, const std::string& stored) const = 0;  // 恒时比较
};

/** 账号存储端口（异步——实现=persistence worker 账号队列；主线程零阻塞）。
 *  传明文 pass：哈希在 worker 做（G9——PBKDF2 12k 次 ≈1~5ms 不进主循环）；不留存明文。 */
struct IAccountStore {
    virtual ~IAccountStore() = default;
    virtual void Register(int fd, const std::string& account, const std::string& pass) = 0;
    virtual void Login(int fd, const std::string& account, const std::string& pass) = 0;
    virtual void QueryStats(int fd, std::uint32_t accountId, std::uint32_t limit) = 0;
};

/** 账号域回流 DTO（persistence worker → 主线程 → DeliverAuth → 33/35 号）。
 *  值语义过界（ADR-9）——fd=E9 寻址快照；rows=35 号载荷源。 */
struct AccountResult {
    enum class Kind : std::uint8_t { Auth, Stats };
    Kind kind = Kind::Auth;
    int fd = 0;                          // E9：发起时 fd 快照（失效=丢+计数）
    bool ok = false;
    AuthReason reason = AuthReason::Ok;  // 33 号 reason 字节
    std::uint32_t accountId = 0;         // 非 pid（E7）——客户端绑定身份用
    std::uint8_t mode = 0;               // 0 guest / 1 account
    std::string name;                    // 账号名（回执显示）
    // —— 35 号载荷源（kind=Stats）——
    struct StatRow {
        std::uint32_t matchId = 0;
        std::uint8_t kills = 0;
        std::uint8_t deaths = 0;
        std::uint64_t endedAt = 0;
    };
    std::vector<StatRow> rows;           // ended_at 倒序
};

/** 账号回执出站口（IWireSink 的账号域兄弟——独立 Fanout，域隔离不混 MatchEvent 流） */
struct IAuthResultSink {
    virtual ~IAuthResultSink() = default;
    virtual void DeliverAuth(const std::vector<AccountResult>& results) = 0;
};

} // namespace sightline::application
