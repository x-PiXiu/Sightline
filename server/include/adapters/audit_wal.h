// audit_wal.h —— ep08 步骤 5：审计/终局 WAL（append-only 行日志·文件 IO）
// 服务两处（§5.3 定案）：①full 模式 mysql 写失败的 Must 记录溢写（SpillToWal 真执行者）
// ②lite 模式审计落盘（§5.7.2——审计不丢 doctrine 穿透 lite）。full 启动 ReplayWal 补写 MySQL。
// 行格式（零依赖纯文本·转义只处理分隔符）：
//   A\t<ver>\t<source>\t<digest>\t<createdUtcMs>\t<note>
//   E\t<matchId>\t<cfgVer>\t<endedUtcMs>\t<name>:<kills>:<deaths>,...
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "application/config_views.h"   // AuditRecord

namespace adapters {

struct WalMatchResult {                   // E 行反序列化产物
    std::string matchId;
    std::string configVersion;
    std::uint64_t endedAtUtcMs = 0;
    struct Row { std::string name; int kills = 0; int deaths = 0; };
    std::vector<Row> rows;
};

class AuditWal {
public:
    explicit AuditWal(std::string path);

    bool AppendAudit(const sightline::application::AuditRecord& r);   // A 行
    bool AppendMatchResults(const WalMatchResult& m);                 // E 行
    std::vector<std::string> ReadAuditLines() const;                 // replay 用（原始 A 行）
    std::vector<WalMatchResult> ReadMatchResults() const;            // replay 用（E 行）
    void Truncate();                                // replay 成功后清空
    const std::string& Path() const { return path_; }
    std::size_t LineCount() const;                  // 横幅/诊断用

    // 纯函数（可单测）：编/解一行
    static std::string EncodeAudit(const sightline::application::AuditRecord& r);
    static bool DecodeAudit(const std::string& line, sightline::application::AuditRecord& out);
    static std::string EncodeMatch(const WalMatchResult& m);
    static bool DecodeMatch(const std::string& line, WalMatchResult& out);

private:
    bool AppendLine(const std::string& line);
    std::string path_;
};

} // namespace adapters
