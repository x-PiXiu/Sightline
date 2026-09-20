// i_account_repository.h —— 账号仓库端口（服务端 02 文档：第三个端口的诞生）
//
// 诞生依据：全项目"接口只在需要第二实现时才建"——现在出现第二实现：
//   内存 fake（单测） / MySqlAccountRepository（运行）。

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sightline::app {

struct AccountRecord
{
    std::uint64_t accountId = 0;      // 0 = 游客/未入库
    std::string   account;
    std::string   nickname;
    std::string   passHash;           // SHA-256(密码+固定盐)
    std::int32_t  wins   = 0;
    std::int32_t  losses = 0;
    std::int32_t  kills  = 0;
    std::int32_t  deaths = 0;
};

class IAccountRepository
{
public:
    virtual ~IAccountRepository() = default;

    /** 按登录名取账号（含战绩汇总）；不存在返回 nullopt */
    virtual std::optional<AccountRecord> load(const std::string& account) = 0;

    /** 注册：account 唯一；重名/非法返回 false */
    virtual bool create(const AccountRecord& record) = 0;

    /** 战绩累加（结算时调用，D3 起使用） */
    virtual void addStats(std::uint64_t accountId, int wins, int losses, int kills, int deaths) = 0;
};

} // namespace sightline::app
