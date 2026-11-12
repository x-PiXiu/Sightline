// match_session 实现 —— M3：开始对局 + 成员离场；M4：实体暂存 + 快照组装；M3：权威推算（R12）
#include "match/match_session.h"
#include <algorithm>
#include <cmath>

namespace sightline::match {
namespace {
    // 最小合法性钳制（M3 版=和解完整性保险，非反作弊——完整校验与踢人 ep08 落地）：
    // 界 = 最大合法速度 × 单帧最大间隔。按"一条输入最多携带一个帧间隔的位移"设界，
    // 故意不用服务器测得的到达间隔——延迟滑块会让输入突发到达（间隔 1~5ms 但各含 ~16ms 位移），
    // 按到达间隔钳制会误伤诚实移动、刷爆客户端和解。
    constexpr float kMaxSpeedUuPerSec = 1000.f;   // uu/s（UE 厘米制；行走 450 / 冲刺 ~700，留裕量）
    constexpr float kMaxInputGapSec   = 0.1f;     // 100ms：帧率跌到 10fps 的单帧位移也放行
    // 下落专用界（2026-11-04 坠楼段拉扯复盘）：诚实 CMC 重力 980 长坠可达 3000+uu/s，原 3D 合模长
    // 把"奔跑+下落"合成一个预算 → 长坠期 dz 逐拍被钳 → 权威位落后 → 和解持续回拉。只放宽 dz<0，
    // 上升维持原界（跳跃 420 远低于界，防飞天）；水平界不变（传送黑客打不动权威位）。
    constexpr float kMaxFallSpeedUuPerSec = 4000.f;
}

MatchSession::MatchSession(std::uint32_t rngSeed)
    : rng_(rngSeed ? rngSeed : std::random_device{}()) {}

std::vector<MatchEvent> MatchSession::StartMatch(std::uint32_t roomId, std::vector<RosterEntry> roster) {
    roomId_ = roomId;
    bActive_ = true;
    // 局级状态开局全量复位（2026-11-03 联调根因）：服务进程常驻、MatchSession 跨局复用——上一局的
    // 终局三件套（击杀上限/AbortMatch 置位）若漏复位，新局首拍即被 Tick 判"已结束"停用，且 UpdateEntity
    // 终局门静默丢全部上报 → 实体永不激活 → Fire 静默返回（现场症状=开枪零回包）
    bEnded_ = false;
    endReason_ = 0;
    snapshotStopAtMs = 0;
    entities_.clear();
    for (std::size_t i = 0; i < roster.size(); ++i) {
        roster[i].spawnIndex = static_cast<std::uint8_t>(i);   // 出生点 = 名册序（v3 契约规则 7）
        Entity E;
        E.playerId = roster[i].playerId;                       // 位置先落原点——首个 19 上报覆盖
        E.spawnIx = static_cast<std::uint8_t>(i);              // ep07：重生选点回传序号（24 号）
        E.weapon.hp = static_cast<float>(maxHp_);              // ep08：血量取配置表
        E.weaponType = 0;                                      // ep08：开局默认 Rifle
        // ep07 开局锚定（2026-11-03 用户实锤"开局按 W 被拉出地图"）：PIE 大厅/对局同关卡重载——
        // 切图前旧世界 Character 的 19 号比新角色早到 1~2s，"首报即真值"会把旧角色站位（可能远离
        // 本局出生点上千米）写成权威位；新角色瞬移出生后被判超速钳制 → 每次和解都往旧位拉。
        // 有合同出生点时复用重生握手（rebasePending）：首条上报须落在锚点 100cm 内才激活——
        // 旧包天然被过滤，权威位从合同出生点开始（与客户端 GameMode 名册序落位同源）；
        // 空合同（单测/无图环境）保持旧行为：首报即真值。
        if (!geometry_.spawnPoints.empty()) {
            const auto& sp = geometry_.spawnPoints[std::min<std::size_t>(
                E.spawnIx, geometry_.spawnPoints.size() - 1)];
            // 出生点钳入活动区（2026-11-04 03:20 局实锤：界外出生点 → 24 号传出去/首拍钳回来
            // =重生即拉扯）。spawnX 同步=钳后值——锚定门与 24 号传送坐标同源；界外本身由
            // 适配层启动告警 + 烘焙红字拦（数据修正在数据入口）。
            E.x = E.spawnX = std::clamp(sp.x, playMinX_, playMaxX_);
            E.y = E.spawnY = std::clamp(sp.y, playMinY_, playMaxY_);
            E.z = E.spawnZ = sp.z;
            E.yaw = sp.yawDeg;
            E.rebasePending = true;
        }
        entities_.push_back(E);
    }
    std::vector<MatchEvent> out;
    out.push_back({MatchEvent::Type::MatchStart, roomId_, std::move(roster), 0});
    return out;
}

void MatchSession::SetShooterRtt(std::uint32_t playerId, std::uint16_t rttMs) {
    for (Entity& E : entities_)
        if (E.playerId == playerId) { E.shooterRtt = rttMs; return; }
    // 未知 pid：静默忽略（与 UpdateEntity 同纪律）
}

const Entity* MatchSession::EntityById(std::uint32_t pid) const {
    for (const Entity& E : entities_)
        if (E.playerId == pid) return &E;
    return nullptr;
}

Entity* MatchSession::FindEntity(std::uint32_t pid) {  // 声明早在头文件——首个定义（Reload 起用）
    for (Entity& E : entities_)
        if (E.playerId == pid) return &E;
    return nullptr;
}

void MatchSession::AbortMatch() {                      // ep07：终局中止（❗12——最后真人离开）
    bActive_ = false;
    bEnded_ = true;
    entities_.clear();
}

std::vector<MatchEvent> MatchSession::RemovePlayer(std::uint32_t playerId) {
    std::vector<MatchEvent> out;
    if (!bActive_) return out;
    for (auto it = entities_.begin(); it != entities_.end(); ++it) {
        if (it->playerId == playerId) {
            entities_.erase(it);
            out.push_back({MatchEvent::Type::EntityLeave, roomId_, {}, playerId});
            break;
        }
    }
    return out;
}

bool MatchSession::IsActive(std::uint32_t playerId) const {
    for (const Entity& E : entities_) if (E.playerId == playerId) return true;
    return false;
}

std::vector<MatchEvent> MatchSession::UpdateEntity(std::uint32_t playerId, float x, float y, float z,
                                float yaw, float pitch, std::uint8_t flags, std::uint32_t seq,
                                std::uint64_t recvMs) {
    if (bEnded_) return {};                    // ep07：终局静默（❗5）——ADR-24：统一事件流签名
    for (Entity& E : entities_) {
        if (E.playerId != playerId) continue;
        if (E.bDead) {                                     // ep07：死亡冻结（❗13——只推增量基准防旧包堆积）
            E.lx = x; E.ly = y; E.lz = z;
            return {};
        }
        if (E.rebasePending) {                             // ep07：重生握手（❗9）
            // ⚠ 开局首报同吃此门：客户端必须先到 15 号出生点（合同点）再发 19——远离出生点的
            //   假位置在此静默拒、实体永不入快照（2026-11-07 实机验证轮实锤：live_chain_test.py
            //   首版上报 (16.5,0) 排查一小时即此坑）。拒=正确防御（防旧包污染基准），坑在"静默"。
            //   排查锚：快照找不到自实体 + 换枪/开火一律被门拒（原因=未激活）。
            const float dx0 = x - E.spawnX, dy0 = y - E.spawnY;
            if (dx0 * dx0 + dy0 * dy0 < 100.f * 100.f) {   // 出生点附近首条 → 重定基准放行
                E.rebasePending = false;
            } else {
                // 在途旧报（死亡位置）→ 只拒不改基准（2026-11-04 拉扯第五轮复盘）：此处曾顺手推
                // lx/ly/lz，把重生落位刚重置的 spawn 基准污染回尸体位——放行首拍 dx=上报−尸体位
                // 是全程距离 → 被 300uu 钳制 → 权威位过冲出生点 → 17 号逐拍带误差硬拉。
                // 开局路径不受影响（首报走 bHasReport=false 采纳分支不读 lx），所以只有"重生后"病。
                return {};
            }
        }
        if (!E.bHasReport) {
            E.x = x; E.y = y; E.z = z;                       // 首报：权威位=上报位（本地生成点即真值）
            E.bHasReport = true;                             // 实体激活（此后入快照）
        } else {
            float dx = x - E.lx, dy = y - E.ly, dz = z - E.lz;   // 客户端声明的本拍位移
            // seq 间隙感知：丢帧时 dx 含多帧位移，bound 按实际间隔倍数放大（cap 3x 防刷）
            // 时间版（ep06 补）：seq 是条数计数器不反映真实间隔——客户端 GameThread 冻结后恢复的
            // 首条上报 gap=1 但位移=冻结时长×速度（500ms→200uu），纯 seq 版误钳诚实玩家 → 和解回拉
            // （2026-10-31 用户实测"按 W 被往后拉"的根因）。bound 取两版 max：seq 版防 TCP 攒批、
            // 时间版放行长间隔诚实位移（cap 0.6s 防断线重连瞬移）。
            const std::uint32_t gap = (seq > E.ackedSeq) ? (seq - E.ackedSeq) : 1u;
            const float gapScale = std::min(static_cast<float>(gap), 3.0f);
            const float seqBound = kMaxSpeedUuPerSec * kMaxInputGapSec * gapScale;
            float timeBound = 0.f;
            if (recvMs != 0 && E.lastRecvMs != 0 && recvMs > E.lastRecvMs)
                timeBound = kMaxSpeedUuPerSec *
                            std::min(static_cast<float>(recvMs - E.lastRecvMs) / 1000.f, 0.6f);
            const float bound = std::max(seqBound, timeBound);
            // 水平/垂直分开钳：水平维持原界；垂直下落放宽到终端速度量级（见 kMaxFallSpeedUuPerSec 注）
            const float boundV = (dz < 0.f)
                ? std::max(bound, kMaxFallSpeedUuPerSec * kMaxInputGapSec * gapScale)
                : bound;
            const float lenH2 = dx * dx + dy * dy;
            if (lenH2 > bound * bound) {                     // 超界钳回界上：保方向弃模长（传送黑客打不动权威位）
                const float k = bound / std::sqrt(lenH2);
                dx *= k; dy *= k;
            }
            if (std::fabs(dz) > boundV) {
                dz = (dz > 0.f) ? boundV : -boundV;
            }
            E.x += dx; E.y += dy; E.z += dz;                 // 权威=上次权威+钳制增量（诚实路径与上报恒等）
        }
        // 活动区钳制（2026-11-04 审计 A 级收编·原 ep09 登记预支）：权威位 x/y 钳回合同地面——
        // 对全体实体生效（真人+人机；此前仅人机适配器钳目标=边界形同虚设）。
        // 落点=权威推进之后、历史环 push 之前——时间线/快照/回溯采样自动一致（与超速钳制同管线）。
        // 客户端和解尾款平滑（6f2ca89）把回拉变成滑行，边界执法此时才可行；z 不钳（坠落合法）。
        // 界=SetGeometry 从合同地板盒推导（数据变了规则跟着走——03:20 局实锤：出生点挪到界外
        // 而常量钉 ±2000 → 24 号传出去/钳制拽回来=重生即拉扯）。
        E.x = std::clamp(E.x, playMinX_, playMaxX_);
        E.y = std::clamp(E.y, playMinY_, playMaxY_);
        E.lx = x; E.ly = y; E.lz = z;                        // 增量基准永远=客户端声明轨迹
        E.lastRecvMs = recvMs;                               // 钳制时间版基准（下一次间隔）
        E.yaw = yaw; E.pitch = pitch;                        // 朝向无动力学：末次上报即权威
        E.flags = flags;
        E.ackedSeq = std::max(E.ackedSeq, seq);              // 确认序号只进不退（乱序防御 · R11）
        // ep06：历史环 push（钳后权威位+服务器接收时刻·单钟 ADR-1——回溯判定的真相时间线）
        E.history[E.histHead] = {recvMs, E.x, E.y, E.z};
        E.histHead = std::uint8_t((E.histHead + 1) % kEntityHistoryCap);
        E.histCount = std::uint8_t(std::min<int>(E.histCount + 1, kEntityHistoryCap));
        return {};
    }
    // 未知 pid：不在局者上报无效——静默忽略（服务器的"信任但只转发"不等于"来者不拒"）
    return {};
}

std::vector<MatchEvent> MatchSession::TickImpl(std::uint64_t nowMs) {
    std::vector<MatchEvent> out;
    if (!bActive_) return out;
    // —— ep07：死亡 → 重生倒计时处理（服务器选点=离最近活敌最远·D7-03）——
    for (Entity& E : entities_) {
        if (!E.bDead || nowMs < E.respawnAtMs) continue;
        E.bDead = false;
        E.weapon.hp = static_cast<float>(maxHp_);             // ep08：配置表血量
        E.weapon.magUsed = 0;
        E.weapon.reloadUntilMs = 0;
        E.weapon.lastFireMs = 0;                             // ep08 D7 连带：射速钟 / 首发标记一并复位
        E.weapon.bHasFired = false;
        E.weaponType = 0;                                     // ep08 D7 连带：重生回 Rifle
        E.bornAtMs = nowMs;                              // 命中候选过滤锚（❗8）
        E.invulnUntilMs = nowMs + deathmatch_.invulnMs;   // 出生保护（D7-03·v1.6 配置表）
        E.rebasePending = true;                          // 重生握手（❗9）
        E.histCount = 0; E.histHead = 0;                 // 历史环清空——死前轨迹不得跨越重生（❗8）
        // 选点=合同出生点均匀随机（2026-11-04 用户令）+排除已占用（同轮补丁：随机不排位→人机
        // 可重生在玩家点位上="还没移动就有人机叠在我身上"）。候选=150cm 内无活人的点；全占回退
        // 全员随机（永不卡死）。落位仍钳入活动区（界外出生点数据由启动告警/烘焙红字拦）。
        if (!geometry_.spawnPoints.empty()) {            // 有合同点：服务器权威落位（人机首报同源）
            std::vector<std::size_t> freeIx;
            for (std::size_t i = 0; i < geometry_.spawnPoints.size(); ++i) {
                const auto& sp = geometry_.spawnPoints[i];
                bool occupied = false;
                for (const Entity& O : entities_) {
                    // 排除重生者本人：外层循环此处已 bDead=false 且仍站在尸体位（=上一个点）——
                    // 不排除则自己占自己的候选点（尸体位在出生点旁时整列被排空→回退全员随机）
                    if (O.playerId == E.playerId || !O.bHasReport || O.bDead) continue;
                    const float dxx = sp.x - O.x, dyy = sp.y - O.y;
                    if (dxx * dxx + dyy * dyy < deathmatch_.spawnClearRadiusCm * deathmatch_.spawnClearRadiusCm) { occupied = true; break; }
                }
                if (!occupied) freeIx.push_back(i);
            }
            if (freeIx.empty())
                for (std::size_t i = 0; i < geometry_.spawnPoints.size(); ++i) freeIx.push_back(i);
            std::uniform_int_distribution<std::size_t> pick(0, freeIx.size() - 1);
            const std::size_t picked = freeIx[pick(rng_)];   // 随机选中序（spawnIx/24 号回传语义不变）
            const auto& sp = geometry_.spawnPoints[picked];
            E.x = std::clamp(sp.x, playMinX_, playMaxX_); // 界外出生点钳入活动区（同 StartMatch——
            E.y = std::clamp(sp.y, playMinY_, playMaxY_); //  24 号带钳后坐标=客户端传送不再被拽回）
            E.z = sp.z;
            E.yaw = sp.yawDeg;
            E.spawnIx = static_cast<std::uint8_t>(picked);  // 选中序随 24 号回传（比分/回放语义）
        } else {                                          // 无合同点：原位复活（ep05 兼容路径）
            E.x = E.spawnX = E.lx; E.y = E.spawnY = E.ly; E.z = E.spawnZ = E.lz;
        }
        E.spawnX = E.x; E.spawnY = E.y; E.spawnZ = E.z;   // rebase 判定锚（❗9）
        E.lx = E.x; E.ly = E.y; E.lz = E.z;
        MatchEvent r;
        r.type = MatchEvent::Type::Respawn;
        r.roomId = roomId_;
        r.respawnPid = E.playerId;
        r.spawnIndex = E.spawnIx;
        r.invulnMs = deathmatch_.invulnMs;                     // v1.6：配置表（24 号载荷源）
        r.respawnX = E.x; r.respawnY = E.y; r.respawnZ = E.z; r.respawnYawDeg = E.yaw;  // v4.5：权威落位坐标随 24 号下发
        out.push_back(std::move(r));
    }
    // —— ep07：结束判定（真人全部离开由 AbortMatch 直达——此处只管击杀上限）——
    if (bEnded_ && endReason_ == 0) {                      // 击杀上限已在 Fire 置 bEnded_+reason=1
        endReason_ = 1;
    }
    if (bEnded_ && endReason_ != 0 && snapshotStopAtMs == 0) {
        snapshotStopAtMs = nowMs + 2000;                   // Ended 事件只发一次：这里补时刻（Abort 直达路径）
        MatchEvent end;
        end.type = MatchEvent::Type::Ended;
        end.roomId = roomId_;
        end.endReason = endReason_;
        for (const Entity& E : entities_)
            end.scores.push_back({E.playerId, static_cast<std::uint16_t>(E.kills),
                                  static_cast<std::uint16_t>(E.deaths)});
        out.push_back(std::move(end));
    }
    if (bEnded_ && snapshotStopAtMs != 0 && nowMs >= snapshotStopAtMs) {
        bActive_ = false;                                  // 结算背景 2s 用毕停快照（❗5）
        return out;
    }
    MatchEvent E{MatchEvent::Type::Snapshot, roomId_, {}, 0};
    E.entities.reserve(entities_.size());
    for (const Entity& Src : entities_) {
        if (!Src.bHasReport) continue;                       // 未上报者不入快照（避免 (0,0,0) 幻影出生）
        const std::uint8_t flags2 = static_cast<std::uint8_t>(
            Src.flags | (Src.bDead ? 0x02 : 0x00) |
            (nowMs < Src.invulnUntilMs ? 0x04 : 0x00) |
            ((Src.weaponType & 0x03u) << 3));             // ep08 v4.8：bit3~4=weaponType（wire 零增长）
        E.entities.push_back({Src.playerId, Src.x, Src.y, Src.z, Src.yaw, Src.pitch,
                              flags2, Src.ackedSeq, Src.weaponType});
    }
    out.push_back(std::move(E));
    return out;
}

// ===== 自主换弹（2026-11-05 用户令·29 号）：换弹钟权威在服务器，客户端只发意图 =====
bool MatchSession::Reload(std::uint32_t playerId, std::uint64_t nowMs) {
    if (!bActive_ || bEnded_) return false;                // 对局外/终局
    Entity* E = FindEntity(playerId);
    if (!E || !E->bHasReport) return false;                // 未激活=打不了也换不了
    if (E->bDead) return false;                            // 死亡期（尸体无手）
    if (nowMs < E->weapon.reloadUntilMs) return false;     // 已在换弹（重复 R 忽略）
    if (E->weapon.magUsed == 0) return false;              // 满匣无须换
    E->weapon.reloadUntilMs = nowMs + weaponTable_[E->weaponType].reloadMs;  // ep08 D2：取当前枪配置
    return true;
}

// ===== ep08 v4.8：配置表 / 换枪状态机 =====
void MatchSession::SetConfig(const GameConfig& c) {
    // 只取本用例执行的字段（ISP）：武器/血量/死亡规则/回溯钳制；formula/bot 归各自消费者。
    weaponTable_ = c.weapons;
    weaponCount_ = (c.weaponCount == 0 || c.weaponCount > 4) ? 2 : c.weaponCount;
    maxHp_ = c.maxHp == 0 ? 100 : c.maxHp;
    deathmatch_ = c.deathmatch;
    rewind_ = c.rewind;
    if (!c.version.empty()) configVersion_ = c.version;   // ep09 G4：版本随快照（空=未注入·兼容）
}

std::uint8_t MatchSession::WeaponTypeOf(std::uint32_t playerId) const {
    for (const Entity& E : entities_)
        if (E.playerId == playerId) return E.weaponType;
    return 0;
}

bool MatchSession::SwitchWeapon(std::uint32_t playerId, std::uint8_t type, std::uint64_t, std::string* why) {
    // 拒绝原因做出参（用例层零日志——编排/落日志归适配器；2026-11-07 实机验证轮补外显）
    const auto Set = [why](const char* r) { if (why) *why = r; };
    if (!bActive_ || bEnded_) { Set("对局门"); return false; }
    if (type >= weaponCount_) { Set("类型越界"); return false; }   // 契约锁：只认配置表里存在的枪
    Entity* E = FindEntity(playerId);
    if (!E) { Set("无实体"); return false; }
    if (!E->bHasReport) { Set("未激活(无19上报)"); return false; }
    if (E->bDead) { Set("死亡期"); return false; }
    if (E->weaponType == type) return true;                 // 幂等：重复按同一数字键无副作用
    E->weaponType = type;
    E->weapon.magUsed = 0;                                 // A4：新枪满匣
    E->weapon.reloadUntilMs = 0;                           // A4：旧枪在途换弹取消
    E->weapon.lastFireMs = 0;                              // A4 漏项：射速钟清零，换枪首帧不被挡
    E->weapon.bHasFired = false;
    return true;
}

// ===== M5：开火裁决（D5 系列；规则在 fire_judge.h 纯决策头，此处只编排）=====

void MatchSession::SetGeometry(const MapGeometry& geometry) {
    geometry_ = geometry;                                    // 构建期烘焙注入（D5-08v2 拒绝运行时上报）
    // 活动区从合同推导（2026-11-04 审计 A 级升级）：地板=足印最大的遮挡盒（hx*hy 最大）——
    // 多地图/挪出生点后规则随数据走，不再拍常量。无盒（缺省合同）回退 kPlayAreaHalfUu。
    float bestArea = -1.f;
    for (const auto& b : geometry_.occluders.boxes) {
        const float area = b.hx * b.hy;
        if (area > bestArea) {
            bestArea = area;
            playMinX_ = b.cx - b.hx; playMaxX_ = b.cx + b.hx;
            playMinY_ = b.cy - b.hy; playMaxY_ = b.cy + b.hy;
        }
    }
}

std::vector<MatchEvent> MatchSession::FireImpl(std::uint32_t shooterPid, std::uint64_t nowMs) {
    std::vector<MatchEvent> out;
    if (!bActive_) return out;                               // ① 在对局中？
    Entity* shooter = nullptr;
    for (Entity& E : entities_)
        if (E.playerId == shooterPid) { shooter = &E; break; }
    if (!shooter || !shooter->bHasReport) return out;        // ① 未上报=位置未知，判不了

    // ep08：per-weapon 参数唯一真相源（配置表副本；未注入时逐字段等于 ep07 常量）
    const WeaponParams wp = weaponTable_[shooter->weaponType];
    // ②③④ 武器门（换弹含到期补满 / 弹匣空触发换弹 / 射速容差——fire_judge 纯函数）
    const FireGate gate = ConsumeWeaponGate(shooter->weapon, nowMs, wp);
    if (gate != FireGate::Ok) {                              // 拒收回传（v4.6：bAccepted=false + gate 随 21 号广播）
        MatchEvent ev;
        ev.type = MatchEvent::Type::FireResult;
        ev.roomId = roomId_;
        // v4.8.1：gate=换弹中 → 带服务器换弹钟剩余毫秒（客户端对齐本地钟根治双钟漂移——
        //  客户端补满后连发全吃服务器换弹尾·2026-11-07 服务器日志 13 连拒实锤）
        // ⚠ 指定初始化器（2026-11-07 教训）：逐位聚合初始化曾把 remaining 错落进 victimHp 位
        //  （字段追加在尾部·9 值错位）→ 回包剩余恒 0·客户端对齐失效——按名初始化永不错位
        ev.fire = FireResultData{
            .shooterPid = shooterPid, .bHit = false, .bAccepted = false,
            .gate = static_cast<std::uint8_t>(gate),
            .reloadRemainingMs = static_cast<std::uint16_t>(
                gate == FireGate::Reloading
                    ? std::min<std::uint64_t>(65535, shooter->weapon.reloadUntilMs - nowMs)
                    : 0)};
        out.push_back(std::move(ev));
        return out;
    }

    // ⓪ 存活门（ep07）：死亡/终局开火拒绝（客户端镜像同款）——gate=Dead（2026-11-03 实锤：
    // 此前复用 ConsumeWeaponGate 的返回值，死亡瞬间武器门恰为 Ok → 日志打出"[射击拒绝] 原因=Ok"自相矛盾）
    if (shooter->bDead || bEnded_) {
        MatchEvent ev;
        ev.type = MatchEvent::Type::FireResult;
        ev.roomId = roomId_;
        ev.fire = FireResultData{.shooterPid = shooterPid, .bAccepted = false,
                                 .gate = static_cast<std::uint8_t>(FireGate::Dead)};
        out.push_back(std::move(ev));
        return out;
    }
    // 开火即破无敌（2026-11-04 用户令·CS/PUBG 惯例）：出生保护只防"被打"不保护"打人"——
    // 否则重生者带着半隐身闪烁+无敌窗贴脸输出（"还没显示出来就被打死"因果链一环）
    shooter->invulnUntilMs = 0;
    // ⑤ hitscan 求交（ep06 回溯）：判定时刻 = 请求到达 − 射手 RTT（钳 rewind_.maxRewindMs·防伪造 ep09 预埋），
    //    目标位姿查其权威位时间线在该时刻的插值——服务器承认射手眼睛看到的世界
    const std::uint64_t judgeAtMs =
        nowMs - std::min<std::uint64_t>(shooter->shooterRtt, rewind_.maxRewindMs);
    const Vec3 o = RayOriginFromEntity(shooter->x, shooter->y, shooter->z, shooter->yaw);
    const Vec3 d = DirectionFromYawPitch(shooter->yaw, shooter->pitch);
    std::vector<EntityPose> others;
    std::vector<EntityPose> invulnOnly;   // 仅因无敌被挡（2026-11-04：判定语义不变，诊断不再无声）
    others.reserve(entities_.size());
    for (const Entity& E : entities_) {
        if (E.playerId == shooterPid || !E.bHasReport) continue;
        if (E.bDead) continue;                                   // ep07：死者不入候选（❗4）
        if (judgeAtMs < E.bornAtMs) continue;                    // ep07：重生前的旧影子打不中（❗8）
        const auto p = SampleHistoryAt(E.history, E.histCount, E.histHead, judgeAtMs);
        if (!p) continue;
        if (nowMs < E.invulnUntilMs) {
            invulnOnly.push_back({E.playerId, p->x, p->y, p->z});   // 无敌期：不可命中，但计入最近候选
            continue;
        }
        others.push_back({E.playerId, p->x, p->y, p->z});
    }
    const FireVerdict v = CastRay(o, d, geometry_.occluders, others, geometry_.hitbox);

    // miss 诊断（可观测性）：最近的候选（回溯采样位到射线的垂距）——判定行可读"目标在哪个方位、偏了多少"。
    // 无敌目标（invulnOnly）一并参选：完美瞄准打在重生保护上不再是"无声 miss"（判定行带 [无敌] 旗）
    std::uint32_t nearPid = 0; float nearPerp = 0.f; Vec3 nearPos{}; bool nearInvuln = false;
    if (!v.hit) {
        float best = 1e9f;
        const auto scan = [&](const std::vector<EntityPose>& list, bool bInv) {
            for (const auto& O : list) {
                const Vec3 w{O.x - o.x, O.y - o.y, O.z - o.z};
                const float t = w.x * d.x + w.y * d.y + w.z * d.z;
                if (t < 0.f) continue;                           // 身后（射线不打背后目标）
                const Vec3 foot{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
                const float perp = std::sqrt((O.x - foot.x) * (O.x - foot.x) +
                                             (O.y - foot.y) * (O.y - foot.y) +
                                             (O.z - foot.z) * (O.z - foot.z));
                if (perp < best) {
                    best = perp; nearPid = O.playerId; nearPos = {O.x, O.y, O.z}; nearInvuln = bInv;
                }
            }
        };
        scan(others, false);
        scan(invulnOnly, true);
        nearPerp = best < 1e8f ? best : 0.f;
        if (nearPid == 0) { nearPerp = 0.f; nearInvuln = false; }
    }

    // ⑥ 结算扣血（ep07：hp≤0 → 死亡状态机启用）；顺手捕获受害实体判定时刻位（诊断日志用）
    std::uint8_t victimHp = 0;
    float victimX = 0.f, victimY = 0.f, victimZ = 0.f;
    if (v.hit) {
        for (Entity& E : entities_) {
            if (E.playerId != v.victimPid) continue;
            // 收口②：伤害走策略端口（nullptr=内建 DamageApply 路径=238 断言原样；
            // 组合根注入后由策略裁决——公式/参数面的热修入口，v1.6 起真接线）
            if (damage_) {
                const HitContext ctx{std::sqrt((v.hitX - o.x) * (v.hitX - o.x) +
                                               (v.hitY - o.y) * (v.hitY - o.y) +
                                               (v.hitZ - o.z) * (v.hitZ - o.z)), /*headshot=*/false};
                E.weapon.hp = damage_->Apply(E.weapon.hp, static_cast<float>(wp.damage), ctx);
            } else {
                E.weapon.hp = DamageApply(E.weapon.hp, static_cast<float>(wp.damage));
            }
            victimHp = static_cast<std::uint8_t>(E.weapon.hp);
            victimX = E.x; victimY = E.y; victimZ = E.z;
            if (E.weapon.hp <= 0.f && !E.bDead) {                // 死亡状态机（D5-02v2 预埋兑现）
                E.bDead = true;
                E.deaths++;
                shooter->kills++;
                E.respawnAtMs = nowMs + deathmatch_.respawnMs;   // v1.6：配置表
                E.rebasePending = true;                          // 重生握手（❗9 预置——落位时确定 spawn 坐标）
                MatchEvent k;
                k.type = MatchEvent::Type::Kill;
                k.roomId = roomId_;
                k.killVictim = E.playerId;
                k.killKiller = shooterPid;
                k.respawnDelayMs = deathmatch_.respawnMs;        // v1.6：配置表（23 号载荷源）
                out.push_back(std::move(k));
                if (shooter->kills >= deathmatch_.killLimit && !bEnded_) {  // 结束判定（D7-02·配置表）
                    bEnded_ = true;
                    endReason_ = 1;                              // Ended 事件由下一拍 Tick 统一产出（比分齐整）
                }
            }
            break;
        }
    }

    // ⑦ 扣弹 + 记时（judgeAtMs 预留：ep06 回溯只换判定时刻，本流水线其余步骤不动）
    ConsumeAmmo(shooter->weapon, nowMs, wp);                // 空匣即上钟（2026-11-05 死窗根除——
                                                             //  原 ③ 下一发才触发=比客户端钟晚整个换弹窗）
    shooter->weapon.lastFireMs = nowMs;
    shooter->weapon.bHasFired = true;

    // ⑧ FireResult 全房间广播事件（含射手自己——FrameWriter 翻译成 21 号）
    MatchEvent ev;
    ev.type = MatchEvent::Type::FireResult;
    ev.roomId = roomId_;
    ev.fire = FireResultData{.shooterPid = shooterPid, .bHit = v.hit, .bAccepted = true,
                             .gate = static_cast<std::uint8_t>(gate),
                             .victimPid = v.victimPid,
                             .hitX = v.hitX, .hitY = v.hitY, .hitZ = v.hitZ,
                             .victimHp = victimHp};   // v4.8.1：按名初始化（reloadRemainingMs 默认 0·Ok 恒 0）
    // ⑧.5 判定链诊断快照（2026-11-04 审计 C 级收编：诊断出域事件——适配层判定行专用，永不入 21 号 wire）
    ev.fireDiag = FireDiagnostics{
        o.x, o.y, o.z, d.x, d.y, d.z,
        v.hit ? std::sqrt((v.hitX - o.x) * (v.hitX - o.x) + (v.hitY - o.y) * (v.hitY - o.y) + (v.hitZ - o.z) * (v.hitZ - o.z)) : 0.f,
        shooter->x, shooter->y, shooter->z,
        victimX, victimY, victimZ,
        geometry_.hitbox.radius, geometry_.hitbox.halfHeight,
        static_cast<std::uint16_t>(nowMs - judgeAtMs)};
    // ⑨ miss 诊断填充（命中时不填——受害位已足够）
    if (!v.hit && nearPid != 0) {
        ev.fireDiag.nearPid = nearPid;
        ev.fireDiag.nearPerp = nearPerp;
        ev.fireDiag.nearX = nearPos.x; ev.fireDiag.nearY = nearPos.y; ev.fireDiag.nearZ = nearPos.z;
        ev.fireDiag.nearInvuln = nearInvuln;
    }
    out.push_back(std::move(ev));
    return out;
}

// ===== ep09 G4（P1-3）：出口唯一——Fire/Tick 包装统一打 configVersion（多 return 的 Impl 单点收口）=====
std::vector<MatchEvent> MatchSession::Fire(std::uint32_t shooterPid, std::uint64_t nowMs) {
    auto out = FireImpl(shooterPid, nowMs);
    if (!configVersion_.empty())
        for (auto& e : out) e.configVersion = configVersion_;
    return out;
}
std::vector<MatchEvent> MatchSession::Tick(std::uint64_t nowMs) {
    auto out = TickImpl(nowMs);
    if (!configVersion_.empty())
        for (auto& e : out) e.configVersion = configVersion_;
    return out;
}

} // namespace sightline::match
