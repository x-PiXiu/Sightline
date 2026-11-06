// audit_wal.cpp —— ep08 步骤 5：WAL 行编解码与文件追加（零外部依赖）
#include "adapters/audit_wal.h"

#include "logger/logger.h"

#include <fstream>
#include <sstream>

namespace adapters {
namespace app = sightline::application;

namespace {
// note 字段转义：\t → 空格、\n → 空格（note 是一行动机文本，无结构语义）
std::string SanitizeField(std::string s) {
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}
// 行内结构符统一禁掉：name 作 k:d 行的分隔载体——含 : 会破反解（右切两刀歧义）
std::string SafeName(std::string s) {
    s = SanitizeField(std::move(s));
    for (char& c : s) if (c == ':') c = '_';
    return s;
}
} // namespace

AuditWal::AuditWal(std::string path) : path_(std::move(path)) {}

std::string AuditWal::EncodeAudit(const app::AuditRecord& r) {
    return "A\t" + SanitizeField(r.version) + "\t" + SanitizeField(r.source) + "\t" +
           SanitizeField(r.diffDigest) + "\t" + std::to_string(r.atUtcMs) + "\t" +
           SanitizeField(r.note);
}

bool AuditWal::DecodeAudit(const std::string& line, app::AuditRecord& out) {
    if (line.rfind("A\t", 0) != 0) return false;
    std::istringstream is(line.substr(2));
    std::string created;
    if (!std::getline(is, out.version, '\t')) return false;
    if (!std::getline(is, out.source, '\t')) return false;
    if (!std::getline(is, out.diffDigest, '\t')) return false;
    if (!std::getline(is, created, '\t')) return false;
    std::getline(is, out.note);                      // 末段可含 tab 已被 Sanitize——整段读
    try { out.atUtcMs = std::stoull(created); } catch (...) { return false; }
    return true;
}

std::string AuditWal::EncodeMatch(const WalMatchResult& m) {
    std::string s = "E\t" + SanitizeField(m.matchId) + "\t" + SanitizeField(m.configVersion) +
                    "\t" + std::to_string(m.endedAtUtcMs) + "\t";
    bool first = true;
    for (const auto& r : m.rows) {
        if (!first) s += ",";
        first = false;
        s += SafeName(r.name) + ":" + std::to_string(r.kills) + ":" + std::to_string(r.deaths);
    }
    return s;
}

bool AuditWal::DecodeMatch(const std::string& line, WalMatchResult& out) {
    if (line.rfind("E\t", 0) != 0) return false;
    std::istringstream is(line.substr(2));
    std::string created, rows;
    if (!std::getline(is, out.matchId, '\t')) return false;
    if (!std::getline(is, out.configVersion, '\t')) return false;
    if (!std::getline(is, created, '\t')) return false;
    if (!std::getline(is, rows)) return false;
    try { out.endedAtUtcMs = std::stoull(created); } catch (...) { return false; }
    out.rows.clear();
    std::size_t b = 0;
    while (b < rows.size()) {
        const auto comma = rows.find(',', b);
        const std::string item = rows.substr(b, comma == std::string::npos ? std::string::npos : comma - b);
        const auto c1 = item.rfind(':');                 // name:k:d —— 从右切两刀（name 可能含 ':'？不允许——Sanitize 未禁：加禁）
        if (c1 != std::string::npos) {
            const auto c2 = item.rfind(':', c1 - 1);
            if (c2 != std::string::npos) {
                WalMatchResult::Row r;
                r.name = item.substr(0, c2);
                try { r.kills = std::stoi(item.substr(c2 + 1, c1 - c2 - 1)); } catch (...) { r.kills = 0; }
                try { r.deaths = std::stoi(item.substr(c1 + 1)); } catch (...) { r.deaths = 0; }
                out.rows.push_back(std::move(r));
            }
        }
        if (comma == std::string::npos) break;
        b = comma + 1;
    }
    return true;
}

bool AuditWal::AppendLine(const std::string& line) {
    std::ofstream f(path_, std::ios::app | std::ios::binary);
    if (!f) {
        LOG_WARNING("[WAL] 打开失败（不丢策略下这是最后防线——检查路径）: " + path_);
        return false;
    }
    f << line << '\n';
    return f.good();
}

bool AuditWal::AppendAudit(const app::AuditRecord& r) { return AppendLine(EncodeAudit(r)); }
bool AuditWal::AppendMatchResults(const WalMatchResult& m) { return AppendLine(EncodeMatch(m)); }

namespace {
std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) out.push_back(line);
    return out;
}
} // namespace

std::vector<std::string> AuditWal::ReadAuditLines() const {
    std::vector<std::string> out;
    for (auto& l : ReadLines(path_))
        if (l.rfind("A\t", 0) == 0) out.push_back(std::move(l));
    return out;
}

std::vector<WalMatchResult> AuditWal::ReadMatchResults() const {
    std::vector<WalMatchResult> out;
    for (const auto& l : ReadLines(path_)) {
        WalMatchResult m;
        if (DecodeMatch(l, m)) out.push_back(std::move(m));
    }
    return out;
}

void AuditWal::Truncate() {
    std::ofstream f(path_, std::ios::trunc | std::ios::binary);   // 截断即清
}

std::size_t AuditWal::LineCount() const {
    return ReadLines(path_).size();
}

} // namespace adapters
