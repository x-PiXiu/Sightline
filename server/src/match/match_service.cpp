// match_service 实现 —— M3：开始对局 + 成员离场；M4：实体暂存 + 快照组装；M3：权威推算（R12）
#include "match/match_service.h"
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
}

std::vector<MatchEvent> MatchService::StartMatch(std::uint32_t roomId, std::vector<RosterEntry> roster) {
    roomId_ = roomId;
    bActive_ = true;
    entities_.clear();
    for (std::size_t i = 0; i < roster.size(); ++i) {
        roster[i].spawnIndex = static_cast<std::uint8_t>(i);   // 出生点 = 名册序（v3 契约规则 7）
        Entity E;
        E.playerId = roster[i].playerId;                       // 位置先落原点——首个 19 上报覆盖
        E.spawnIx = static_cast<std::uint8_t>(i);              // ep07：重生选点回传序号（24 号）
        entities_.push_back(E);
    }
    std::vector<MatchEvent> out;
    out.push_back({MatchEvent::Type::MatchStart, roomId_, std::move(roster), 0});
    return out;
}

void MatchService::SetShooterRtt(std::uint32_t playerId, std::uint16_t rttMs) {
    for (Entity& E : entities_)
        if (E.playerId == playerId) { E.shooterRtt = rttMs; return; }
    // 未知 pid：静默忽略（与 UpdateEntity 同纪律）
}

const Entity* MatchService::EntityById(std::uint32_t pid) const {
    for (const Entity& E : entities_)
        if (E.playerId == pid) return &E;
    return nullptr;
}

void MatchService::AbortMatch() {                      // ep07：终局中止（❗12——最后真人离开）
    bActive_ = false;
    bEnded_ = true;
    entities_.clear();
}

std::vector<MatchEvent> MatchService::RemovePlayer(std::uint32_t playerId) {
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

bool MatchService::IsActive(std::uint32_t playerId) const {
    for (const Entity& E : entities_) if (E.playerId == playerId) return true;
    return false;
}

void MatchService::UpdateEntity(std::uint32_t playerId, float x, float y, float z,
                                float yaw, float pitch, std::uint8_t flags, std::uint32_t seq,
                                std::uint64_t recvMs) {
    if (bEnded_) return;                                   // ep07：终局静默（❗5）
    for (Entity& E : entities_) {
        if (E.playerId != playerId) continue;
        if (E.bDead) {                                     // ep07：死亡冻结（❗13——只推增量基准防旧包堆积）
            E.lx = x; E.ly = y; E.lz = z;
            return;
        }
        if (E.rebasePending) {                             // ep07：重生握手（❗9）
            const float dx0 = x - E.spawnX, dy0 = y - E.spawnY;
            if (dx0 * dx0 + dy0 * dy0 < 100.f * 100.f) {   // 出生点附近首条 → 重定基准放行
                E.rebasePending = false;
            } else {
                E.lx = x; E.ly = y; E.lz = z;              // 在途旧报（死亡位置）→ 忽略
                return;
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
            const float len2 = dx * dx + dy * dy + dz * dz;
            if (len2 > bound * bound) {                      // 超界钳回界上：保方向弃模长（传送黑客打不动权威位）
                const float k = bound / std::sqrt(len2);
                dx *= k; dy *= k; dz *= k;
            }
            E.x += dx; E.y += dy; E.z += dz;                 // 权威=上次权威+钳制增量（诚实路径与上报恒等）
        }
        E.lx = x; E.ly = y; E.lz = z;                        // 增量基准永远=客户端声明轨迹
        E.lastRecvMs = recvMs;                               // 钳制时间版基准（下一次间隔）
        E.yaw = yaw; E.pitch = pitch;                        // 朝向无动力学：末次上报即权威
        E.flags = flags;
        E.ackedSeq = std::max(E.ackedSeq, seq);              // 确认序号只进不退（乱序防御 · R11）
        // ep06：历史环 push（钳后权威位+服务器接收时刻·单钟 ADR-1——回溯判定的真相时间线）
        E.history[E.histHead] = {recvMs, E.x, E.y, E.z};
        E.histHead = std::uint8_t((E.histHead + 1) % kEntityHistoryCap);
        E.histCount = std::uint8_t(std::min<int>(E.histCount + 1, kEntityHistoryCap));
        return;
    }
    // 未知 pid：不在局者上报无效——静默忽略（服务器的"信任但只转发"不等于"来者不拒"）
}

std::vector<MatchEvent> MatchService::Tick(std::uint64_t nowMs) {
    std::vector<MatchEvent> out;
    if (!bActive_) return out;
    // —— ep07：死亡 → 重生倒计时处理（服务器选点=离最近活敌最远·D7-03）——
    for (Entity& E : entities_) {
        if (!E.bDead || nowMs < E.respawnAtMs) continue;
        E.bDead = false;
        E.weapon.hp = kMaxHp;
        E.weapon.magUsed = 0;
        E.weapon.reloadUntilMs = 0;
        E.bornAtMs = nowMs;                              // 命中候选过滤锚（❗8）
        E.invulnUntilMs = nowMs + kInvulnMs;             // 出生保护（D7-03）
        E.rebasePending = true;                          // 重生握手（❗9）
        E.histCount = 0; E.histHead = 0;                 // 历史环清空——死前轨迹不得跨越重生（❗8）
        std::size_t best = 0; float bestD = -1.f;
        for (std::size_t i = 0; i < geometry_.spawnPoints.size(); ++i) {
            const auto& sp = geometry_.spawnPoints[i];
            float dMin = 1e9f;
            for (const Entity& O : entities_) {
                if (O.playerId == E.playerId || O.bDead || !O.bHasReport) continue;
                const float dx = sp.x - O.x, dy = sp.y - O.y;
                dMin = std::min(dMin, dx * dx + dy * dy);
            }
            if (dMin > bestD) { bestD = dMin; best = i; }
        }
        if (!geometry_.spawnPoints.empty()) {            // 有合同点：服务器权威落位（人机首报同源）
            const auto& sp = geometry_.spawnPoints[best];
            E.x = sp.x; E.y = sp.y; E.z = sp.z;
            E.yaw = sp.yawDeg;
            E.spawnIx = static_cast<std::uint8_t>(best);
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
        r.invulnMs = static_cast<std::uint16_t>(kInvulnMs);
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
        const std::uint8_t flags2 =
            static_cast<std::uint8_t>(Src.flags | (Src.bDead ? 0x02 : 0x00));  // bit1=dead（ep07 表现）
        E.entities.push_back({Src.playerId, Src.x, Src.y, Src.z, Src.yaw, Src.pitch, flags2, Src.ackedSeq});
    }
    out.push_back(std::move(E));
    return out;
}

// ===== M5：开火裁决（D5 系列；规则在 fire_judge.h 纯决策头，此处只编排）=====

void MatchService::SetGeometry(const MapGeometry& geometry) {
    geometry_ = geometry;                                    // 构建期烘焙注入（D5-08v2 拒绝运行时上报）
}

std::vector<MatchEvent> MatchService::Fire(std::uint32_t shooterPid, std::uint64_t nowMs) {
    std::vector<MatchEvent> out;
    if (!bActive_) return out;                               // ① 在对局中？
    Entity* shooter = nullptr;
    for (Entity& E : entities_)
        if (E.playerId == shooterPid) { shooter = &E; break; }
    if (!shooter || !shooter->bHasReport) return out;        // ① 未上报=位置未知，判不了

    // ②③④ 武器门（换弹含到期补满 / 弹匣空触发换弹 / 射速容差 95ms——fire_judge 纯函数）
    const FireGate gate = ConsumeWeaponGate(shooter->weapon, nowMs);
    if (gate != FireGate::Ok) {                              // 静默丢（D5-03）——诊断数据仍回传适配层落日志
        MatchEvent ev;
        ev.type = MatchEvent::Type::FireResult;
        ev.roomId = roomId_;
        ev.fire = FireResultData{shooterPid, false, false, static_cast<std::uint8_t>(gate),
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.push_back(std::move(ev));
        return out;
    }

    // ⓪ 存活门（ep07）：死亡者开火静默丢（客户端镜像同款）
    if (shooter->bDead || bEnded_) {
        MatchEvent ev;
        ev.type = MatchEvent::Type::FireResult;
        ev.roomId = roomId_;
        ev.fire = FireResultData{shooterPid, false, false, static_cast<std::uint8_t>(gate),
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.push_back(std::move(ev));
        return out;
    }
    // ⑤ hitscan 求交（ep06 回溯）：判定时刻 = 请求到达 − 射手 RTT（钳 kMaxRewindMs·防伪造 ep09 预埋），
    //    目标位姿查其权威位时间线在该时刻的插值——服务器承认射手眼睛看到的世界
    const std::uint64_t judgeAtMs =
        nowMs - std::min<std::uint64_t>(shooter->shooterRtt, kMaxRewindMs);
    const Vec3 o = RayOriginFromEntity(shooter->x, shooter->y, shooter->z, shooter->yaw);
    const Vec3 d = DirectionFromYawPitch(shooter->yaw, shooter->pitch);
    std::vector<EntityPose> others;
    others.reserve(entities_.size());
    for (const Entity& E : entities_) {
        if (E.playerId == shooterPid || !E.bHasReport) continue;
        if (E.bDead) continue;                                   // ep07：死者不入候选（❗4）
        if (judgeAtMs < E.bornAtMs) continue;                    // ep07：重生前的旧影子打不中（❗8）
        if (nowMs < E.invulnUntilMs) continue;                   // ep07：无敌期不被命中（出生保护）
        const auto p = SampleHistoryAt(E.history, E.histCount, E.histHead, judgeAtMs);
        if (!p) continue;
        others.push_back({E.playerId, p->x, p->y, p->z});
    }
    const FireVerdict v = CastRay(o, d, geometry_.occluders, others, geometry_.hitbox);

    // ⑥ 结算扣血（ep07：hp≤0 → 死亡状态机启用）；顺手捕获受害实体判定时刻位（诊断日志用）
    std::uint8_t victimHp = 0;
    float victimX = 0.f, victimY = 0.f, victimZ = 0.f;
    if (v.hit) {
        for (Entity& E : entities_) {
            if (E.playerId != v.victimPid) continue;
            E.weapon.hp = DamageApply(E.weapon.hp);
            victimHp = static_cast<std::uint8_t>(E.weapon.hp);
            victimX = E.x; victimY = E.y; victimZ = E.z;
            if (E.weapon.hp <= 0.f && !E.bDead) {                // 死亡状态机（D5-02v2 预埋兑现）
                E.bDead = true;
                E.deaths++;
                shooter->kills++;
                E.respawnAtMs = nowMs + kRespawnMs;
                E.rebasePending = true;                          // 重生握手（❗9 预置——落位时确定 spawn 坐标）
                MatchEvent k;
                k.type = MatchEvent::Type::Kill;
                k.roomId = roomId_;
                k.killVictim = E.playerId;
                k.killKiller = shooterPid;
                k.respawnDelayMs = static_cast<std::uint16_t>(kRespawnMs);
                out.push_back(std::move(k));
                if (shooter->kills >= kKillLimit && !bEnded_) {  // 结束判定（D7-02）
                    bEnded_ = true;
                    endReason_ = 1;                              // Ended 事件由下一拍 Tick 统一产出（比分齐整）
                }
            }
            break;
        }
    }

    // ⑦ 扣弹 + 记时（judgeAtMs 预留：ep06 回溯只换判定时刻，本流水线其余步骤不动）
    shooter->weapon.magUsed++;
    shooter->weapon.lastFireMs = nowMs;
    shooter->weapon.bHasFired = true;

    // ⑧ FireResult 全房间广播事件（含射手自己——FrameWriter 翻译成 21 号）
    MatchEvent ev;
    ev.type = MatchEvent::Type::FireResult;
    ev.roomId = roomId_;
    ev.fire = FireResultData{shooterPid, v.hit, true, static_cast<std::uint8_t>(gate),
                             v.victimPid, v.hitX, v.hitY, v.hitZ, victimHp,
                             o.x, o.y, o.z, d.x, d.y, d.z,
                             v.hit ? std::sqrt((v.hitX - o.x) * (v.hitX - o.x) + (v.hitY - o.y) * (v.hitY - o.y) + (v.hitZ - o.z) * (v.hitZ - o.z)) : 0.f,
                             shooter->x, shooter->y, shooter->z,
                             victimX, victimY, victimZ,
                             geometry_.hitbox.radius, geometry_.hitbox.halfHeight,
                             static_cast<std::uint16_t>(nowMs - judgeAtMs)};
    out.push_back(std::move(ev));
    return out;
}

} // namespace sightline::match
