// test_account_repo.cpp —— IAccountRepository 接口契约测试（fake 实现，零 DB 依赖）
//
// 场景：create → load 往返 / 重复 create 拒绝 / 错误密码 load 成功(密码校验在 Service 层) /
//       addStats 累加正确。真实 MySQL 实现复用同一套用例(接口契约即测试)。

#include "application/ports/i_account_repository.h"
#include <map>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/** 内存 fake——单测专用，模拟"账号唯一 + 战绩累加"的最小语义 */
class InMemoryAccountRepository final : public sightline::app::IAccountRepository
{
public:
    std::optional<sightline::app::AccountRecord> load(const std::string& account) override
    {
        auto it = map_.find(account);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }

    bool create(const sightline::app::AccountRecord& record) override
    {
        if (map_.count(record.account)) return false;           // 唯一约束
        map_[record.account] = record;
        map_[record.account].accountId = next_id_++;
        return true;
    }

    void addStats(std::uint64_t accountId, int w, int l, int k, int d) override
    {
        for (auto& [_, rec] : map_)
        {
            if (rec.accountId == accountId)
            {
                rec.wins += w; rec.losses += l; rec.kills += k; rec.deaths += d;
                return;
            }
        }
    }

private:
    std::map<std::string, sightline::app::AccountRecord> map_;
    std::uint64_t next_id_ = 1000;
};

template <typename Repo>
int runContract(Repo& repo)
{
    sightline::app::AccountRecord rec;
    rec.account = "nova"; rec.nickname = "Nova";
    rec.passHash = std::string(64, 'a');

    if (!repo.create(rec))                    { std::puts("[FAIL] create 新账号失败"); return 1; }
    if (repo.create(rec))                     { std::puts("[FAIL] 重复 create 未拒绝"); return 2; }

    auto loaded = repo.load("nova");
    if (!loaded)                              { std::puts("[FAIL] load 未找到"); return 3; }
    if (loaded->nickname != "Nova")           { std::puts("[FAIL] nickname 不一致"); return 4; }
    if (loaded->accountId == 0)               { std::puts("[FAIL] accountId 未生成"); return 5; }

    repo.addStats(loaded->accountId, 1, 0, 5, 1);
    loaded = repo.load("nova");
    if (!loaded || loaded->wins != 1 || loaded->kills != 5 || loaded->deaths != 1)
                                              { std::puts("[FAIL] addStats 累加错误"); return 6; }

    if (repo.load("ghost"))                   { std::puts("[FAIL] 不存在账号不应 load 到"); return 7; }
    return 0;
}

} // namespace

int main()
{
    InMemoryAccountRepository repo;
    if (int rc = runContract(repo); rc != 0)
    {
        std::printf("[FAIL] InMemoryAccountRepository 契约测试失败 rc=%d\n", rc);
        return rc;
    }
    std::puts("[PASS] IAccountRepository 契约测试通过（fake 实现）");
    return 0;
}
