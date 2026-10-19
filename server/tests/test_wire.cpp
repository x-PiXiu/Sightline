// test_wire.cpp —— 第 4 期 v4 wire 结构体测试（variant 编解码，D4 批准）
// 覆盖：PlayerInput Parse 长度守门/字段还原、EntityStateV2 Encode 29B 字节级布局、
//       MatchStart/EntityLeave Encode 布局、DecodeC2S 未知 msgid 拒绝、StartMatch 空 payload 纪律
// 铁律：不起网络、纯字节断言（契约 v4 = 图 = 代码一致）
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "protocol/messages.h"

using namespace sightline::wire;

static int g_passed = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); std::abort(); } ++g_passed; } while (0)

static std::span<const std::uint8_t> SP(const std::vector<std::uint8_t>& v) {
    return {v.data(), v.size()};
}

int main() {
    std::puts("—— PlayerInput(19) Parse：长度守门 ——（已过)");
    {
        std::vector<std::uint8_t> ok(25, 0);
        ok[0] = 0x07; ok[1] = 0x00; ok[2] = 0x00; ok[3] = 0x00;   // seq=7
        ok[24] = 0x01;                                            // flags=1
        auto m = PlayerInputMsg::Parse(SP(ok));
        CHECK(m.has_value());
        CHECK(m->seq == 7);
        CHECK(m->flags == 1);
        std::vector<std::uint8_t> shortp(24, 0);
        CHECK(!PlayerInputMsg::Parse(SP(shortp)).has_value());    // 24B 拒
        std::vector<std::uint8_t> longp(26, 0);
        CHECK(!PlayerInputMsg::Parse(SP(longp)).has_value());     // 26B 拒
        // float 小端还原：x=1.0f = 0x3F800000
        std::vector<std::uint8_t> f(25, 0);
        const float one = 1.0f;
        std::memcpy(f.data() + 4, &one, sizeof(float));
        auto mf = PlayerInputMsg::Parse(SP(f));
        CHECK(mf.has_value());
        CHECK(mf->x == 1.0f && mf->y == 0.f && mf->z == 0.f);
    }

    std::puts("—— EntityStateV2(17v2) Encode：29B 字节级布局 ——");
    {
        EntityStateV2 e{};
        e.pid = 9; e.x = 1.f; e.y = 2.f; e.z = 3.f; e.yaw = 4.f; e.pitch = 5.f;
        e.flags = 0x01; e.ackedSeq = 42;
        const std::string b = e.Encode();
        CHECK(b.size() == EntityStateV2::kSize);                   // 29B
        CHECK(static_cast<std::uint8_t>(b[0]) == 9);               // pid 小端首字节
        CHECK(static_cast<std::uint8_t>(b[25]) == 42);             // ackedSeq 小端首字节 = 破例新增字段落位
        CHECK(b[24] == 1);                                         // flags @24
        float x;
        std::memcpy(&x, b.data() + 4, sizeof(float));
        CHECK(x == 1.f);
    }

    std::puts("—— MatchStart/EntityLeave Encode 布局 ——");
    {
        MatchStartMsg ms;
        ms.roster.push_back({9, "ab", 0});
        ms.roster.push_back({10, "c", 1});
        const std::string b = ms.Encode();
        CHECK(static_cast<std::uint8_t>(b[0]) == 2);               // 人数=2（低字节）
        CHECK(static_cast<std::uint8_t>(b[1]) == 0);
        CHECK(static_cast<std::uint8_t>(b[4]) == 9);               // pid1 首字节（4B 玩家 @4..7）
        CHECK(static_cast<std::uint8_t>(b[8]) == 2);               // 名长=2（@8——玩家 4B 之后）
        CHECK(b[9] == 'a' && b[10] == 'b');
        CHECK(static_cast<std::uint8_t>(b[11]) == 0);              // 出生点序（@11）
        CHECK(static_cast<std::uint8_t>(b[12]) == 10);             // pid2 首字节（第二条目 @12）
        CHECK(static_cast<std::uint8_t>(b[16]) == 1 && b[17] == 'c' && static_cast<std::uint8_t>(b[18]) == 1);
        const std::string lv = EntityLeaveMsg{7}.Encode();
        CHECK(lv.size() == 4 && static_cast<std::uint8_t>(lv[0]) == 7);
    }

    std::puts("—— DecodeC2S：唯一 switch ——");
    {
        std::vector<std::uint8_t> empty;
        auto sm = DecodeC2S(14, SP(empty));                        // 14 空 payload 合法
        CHECK(sm.has_value() && std::holds_alternative<StartMatchMsg>(*sm));
        std::vector<std::uint8_t> bad14(1, 0);
        CHECK(!DecodeC2S(14, SP(bad14)).has_value());              // 14 非空拒
        std::vector<std::uint8_t> in(25, 0);
        auto pi = DecodeC2S(19, SP(in));
        CHECK(pi.has_value() && std::holds_alternative<PlayerInputMsg>(*pi));
        CHECK(!DecodeC2S(16, SP(in)).has_value());                 // 未迁移 msgid → nullopt（回落旧路径）
        CHECK(!DecodeC2S(99, SP(in)).has_value());                 // 未知 msgid 拒
    }

    std::printf("全部通过：%d 项断言 ✓（v4 wire 结构体，未起网络）\n", g_passed);
    return 0;
}
