// password_pbkdf2.h —— ep09 步骤 5：IPasswordHasher 的 PBKDF2 适配器（ADR-22 机制面）
// OpenSSL EVP_PKCS5_PBKDF2_HMAC·SHA-256·iter=12000（教学口径——注释标注 OWASP 生产 600k 升级路径）
// 盐 16B RAND_bytes·存储串 pbkdf2-sha256$12000$<salt_b64>$<hash_b64>·verify=CRYPTO_memcmp 恒时
#pragma once
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "application/account_ports.h"

namespace adapters {

class Pbkdf2Hasher final : public sightline::application::IPasswordHasher {
public:
    static constexpr std::uint32_t kIterations = 12000;   // P4 待批示默认（注释=OWASP 600k 生产路径）
    static constexpr std::size_t kSaltBytes = 16;
    static constexpr std::size_t kHashBytes = 32;         // SHA-256 输出

    std::string Hash(const std::string& plain) const override {
        std::array<std::uint8_t, kSaltBytes> salt{};
        if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
            return {};                                     // RNG 失败=不可哈希（调用方拒收）
        std::array<std::uint8_t, kHashBytes> key{};
        PKCS5_PBKDF2_HMAC(plain.c_str(),
                          static_cast<int>(plain.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          static_cast<int>(kIterations), EVP_sha256(),
                          static_cast<int>(key.size()), key.data());
        return "pbkdf2-sha256$" + std::to_string(kIterations) + "$" +
               B64_(salt.data(), salt.size()) + "$" + B64_(key.data(), key.size());
    }

    bool Verify(const std::string& plain, const std::string& stored) const override {
        // 解析存储串 → 同参数重导出 → CRYPTO_memcmp 恒时比较（不短路）
        if (stored.rfind("pbkdf2-sha256$", 0) != 0) {
                return false;
        }
        const auto p1 = stored.find('$', 14);
        const auto p2 = stored.find('$', p1 + 1);
        // 修复（diag 实锤）：hash 段后无分隔符——p3 必为 npos，原 `p3==npos 判格式错`=永远拒收
        if (p1 == std::string::npos || p2 == std::string::npos) {
                return false;
        }
        const std::uint32_t iter = static_cast<std::uint32_t>(std::stoul(stored.substr(14, p1 - 14)));
        const std::string saltB64 = stored.substr(p1 + 1, p2 - p1 - 1);
        const std::string hashB64 = stored.substr(p2 + 1);   // 到串尾（B64 padding 结束）
        const auto salt = UnB64_(saltB64);
        const auto expect = UnB64_(hashB64);
        if (salt.empty() || expect.size() != kHashBytes) return false;
        std::array<std::uint8_t, kHashBytes> key{};
        PKCS5_PBKDF2_HMAC(plain.c_str(),
                          static_cast<int>(plain.size()),
                          salt.data(), static_cast<int>(salt.size()),
                          static_cast<int>(iter), EVP_sha256(),
                          static_cast<int>(key.size()), key.data());
        return CRYPTO_memcmp(key.data(), expect.data(), kHashBytes) == 0;   // 恒时比较（侧信道防御）
    }

private:
    static std::string B64_(const std::uint8_t* d, std::size_t n) {
        static const char* T =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((n + 2) / 3 * 4);
        for (std::size_t i = 0; i < n; i += 3) {
            const std::uint32_t v = (d[i] << 16) | (i + 1 < n ? d[i + 1] << 8 : 0) |
                                    (i + 2 < n ? d[i + 2] : 0);
            out += T[(v >> 18) & 63];
            out += T[(v >> 12) & 63];
            out += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
            out += (i + 2 < n) ? T[v & 63] : '=';
        }
        return out;
    }
    static std::vector<std::uint8_t> UnB64_(const std::string& in) {
        static const std::string T =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<std::uint8_t> out;
        std::uint32_t v = 0; int bits = 0;
        for (const char c : in) {
            if (c == '=') break;
            const auto p = T.find(c);
            if (p == std::string::npos) return {};
            v = (v << 6) | static_cast<std::uint32_t>(p);
            bits += 6;
            if (bits >= 8) { bits -= 8; out.push_back(static_cast<std::uint8_t>((v >> bits) & 0xFF)); }
        }
        return out;
    }
};

} // namespace adapters
