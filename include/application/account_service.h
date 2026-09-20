// application/account_service.h —— 账号用例：注册/登录的业务规则（15 号 02 文档）
//
// 规则都在这里（哈希比对/重名拒绝/口令格式），存储经 IAccountRepository 端口。
// 运行于 StorageIO 存储线程（同步阻塞无碍——不在网络线程）。

#pragma once

#include <random>
#include <string>
#include "application/ports/i_account_repository.h"

namespace sightline::app {

class AccountService
{
public:
    struct LoginResult
    {
        bool          ok = false;
        std::string   err;                // no_such_account / bad_password / internal
        AccountRecord record;             // 成功时携带
        std::string   token;              // 会话凭据（成功时生成）
    };

    explicit AccountService(std::shared_ptr<IAccountRepository> repo)
        : repo_(std::move(repo)) {}

    /** 登录：校验口令哈希 → 生成会话 token（调用方负责下发与回投） */
    LoginResult login(const std::string& account, const std::string& passHash)
    {
        LoginResult r;
        auto rec = repo_->load(account);
        if (!rec)                              { r.err = "no_such_account"; return r; }
        if (rec->passHash != passHash)         { r.err = "bad_password";    return r; }
        r.ok = true; r.record = *rec; r.token = generateToken();
        return r;
    }

    /** 注册：重名拒绝；成功即入库（战绩默认 0） */
    bool registerAccount(const std::string& account, const std::string& passHash,
                         const std::string& nickname)
    {
        if (account.empty() || passHash.size() != 64) return false;   // 哈希必须 64 hex
        AccountRecord rec;
        rec.account  = account;
        rec.nickname = nickname.empty() ? account : nickname;
        rec.passHash = passHash;
        return repo_->create(rec);
    }

private:
    /** 会话 token：32 位十六进制随机串（Demo 随机源；生产用 CSPRNG） */
    static std::string generateToken()
    {
        static std::mt19937_64 rng{std::random_device{}()};
        static const char* hex = "0123456789abcdef";
        std::uniform_int_distribution<int> d(0, 15);
        std::string s;
        s.reserve(32);
        for (int i = 0; i < 32; ++i) s += hex[d(rng)];
        return s;
    }

    std::shared_ptr<IAccountRepository> repo_;
};

} // namespace sightline::app
