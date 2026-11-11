// account_service.h —— ep09 步骤 5（刀 A）：账号域用例（G9/ADR-16——预检+异步投递，主线程零重活）
// 职责（整洁架构 §7.6-A 判定树）：
//   ① 合法性预检=输入验证（Guard——正则/长度，规则来自契约 v4.9）
//   ② 异步投递（IAccountStore——哈希+IO 在 persistence worker）
//   ③ 回执寻址字段（fd 快照 E9）在投递时封装进 worker 的处理上下文
// 不做：pid 分配（E7——身份层与在线层解耦）、Redis 会话（刀 B persistence 内 BestEffort）。
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "application/account_ports.h"

namespace sightline::application {

class AccountService {
public:
    struct Deps {
        IPasswordHasher* hasher = nullptr;   // 预检通过后仅用于"非空校验"语义（哈希本体在 worker）
        IAccountStore*   store   = nullptr;  // 异步投递口（nullptr=账号域未装——拒收 Unavailable）
        /** 预检失败即时回执出口（不经 worker——G9：主线程只做微秒级 Guard，结果同步 emit） */
        std::function<void(const AccountResult&)> emit;
    };

    explicit AccountService(Deps d) : d_(d) {}
    /** 装配旋钮（main 组合根：store 在 persistence 装配后回填/emit 在 wiring 构造后接线） */
    void SetStore(IAccountStore* s) { d_.store = s; }
    void SetEmit(std::function<void(const AccountResult&)> e) { d_.emit = std::move(e); }

    /** 31 号注册（成功即自动登录态——33 号 reason=0 带 accountId） */
    bool Register(int fd, const std::string& account, const std::string& pass) {
        if (!d_.store) return Fail_(fd, account, AuthReason::Unavailable);
        if (!ValidAccount_(account) || !ValidPass_(pass))
            return Fail_(fd, account, AuthReason::BadParams);
        d_.store->Register(fd, account, pass);      // 明文过界（值语义）——worker 即哈希不留存
        return true;
    }

    /** 32 号登录（游客 4 号路径零改动并行——身份层不碰在线层） */
    bool Login(int fd, const std::string& account, const std::string& pass) {
        if (!d_.store) return Fail_(fd, account, AuthReason::Unavailable);
        if (!ValidAccount_(account) || !ValidPass_(pass))
            return Fail_(fd, account, AuthReason::BadParams);
        d_.store->Login(fd, account, pass);
        return true;
    }

    /** 34 号战绩查询（只查自己——身份=fd→登录态 accountId 由 wiring 解析后传入，
     *  客户端不传身份防伪造；未登录（accountId=0）=BadParams 前置拒） */
    bool QueryStats(int fd, std::uint32_t accountId, std::uint32_t limit) {
        if (!d_.store) return Fail_(fd, "", AuthReason::Unavailable);
        if (accountId == 0) return Fail_(fd, "", AuthReason::BadParams);   // 未登录查战绩
        if (limit == 0 || limit > 20) limit = 20;                          // 钳制（契约 1B ≤20）
        d_.store->QueryStats(fd, accountId, limit);
        return true;
    }

    // —— 实体层合法性（契约 v4.9：account ^[A-Za-z0-9_]{2,24}$·pass 6~64B）——
    static bool ValidAccount(const std::string& a) { return ValidAccount_(a); }
    static bool ValidPass(const std::string& p) { return ValidPass_(p); }

private:
    static bool ValidAccount_(const std::string& a) {
        if (a.size() < kAccountMinLen || a.size() > kAccountMaxLen) return false;
        for (const char c : a)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_'))
                return false;
        return true;
    }
    static bool ValidPass_(const std::string& p) {
        return p.size() >= kPassMinLen && p.size() <= kPassMaxLen;
    }
    bool Fail_(int fd, const std::string& account, AuthReason r) {
        // 预检失败即时回执（不经 worker——同步 emit，fd 寻址同 E9）
        if (d_.emit) {
            AccountResult res;
            res.kind = AccountResult::Kind::Auth;
            res.fd = fd;
            res.ok = false;
            res.reason = r;
            res.mode = 0;
            res.name = account;
            d_.emit(res);
        }
        return false;
    }

    Deps d_;
};

} // namespace sightline::application
