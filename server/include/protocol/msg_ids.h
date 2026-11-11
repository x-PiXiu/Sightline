// 协议消息枚举 —— 与 docs/protocol/MSG_IDS.md v4.8 保持同步（服务端 ⇆ UE 共同契约）
// 帧格式：[2B len][2B msgid][payload]，小端；len = 2 + payload.size()
#pragma once
#include <cstdint>

namespace protocol {

    enum MsgId : uint16_t {
        MSG_S2C_WELCOME        = 1,  // S→C  [8B serverNowMs]
        MSG_C2S_HEARTBEAT      = 2,  // C→S  [4B seq][8B clientTs]
        MSG_S2C_HEARTBEAT_ACK  = 3,  // S→C  [4B seq][8B clientTs][8B serverTs]
        // —— 第 2 期：登录 + 房间（与 docs/protocol/MSG_IDS.md v2 保持同步）——
        MSG_C2S_LOGIN          = 4,  // C→S  [1B 名长][名字]
        MSG_S2C_LOGIN_RESULT   = 5,  // S→C  [1B 结果码][4B PlayerId]
        MSG_C2S_CREATE_ROOM    = 6,  // C→S  [1B 房名长][房间名]
        MSG_S2C_CREATE_RESULT  = 7,  // S→C  [1B 结果码][4B RoomId]
        MSG_C2S_JOIN_ROOM      = 8,  // C→S  [4B RoomId]
        MSG_S2C_JOIN_RESULT    = 9,  // S→C  [1B 结果码][4B RoomId][1B 人数]
        MSG_C2S_LEAVE_ROOM     = 10, // C→S  (空)
        MSG_S2C_ROOM_LIST      = 11, // S→C  [2B 房间数]{[4B RoomId][1B 人数][1B 房名长][房名][1B 房主名长][房主名]}
        MSG_S2C_ROOM_STATE     = 12, // S→C  [4B RoomId][1B 房主名长][房主名][1B 人数]{[1B 名长][名字]}
        MSG_S2C_KICKED         = 13, // S→C  [1B 原因码]（1 顶号 / 2 登录超时 / 3 服务器关闭）
        // —— 第 3 期：朴素移动同步对局（与 docs/protocol/MSG_IDS.md v3 保持同步）——
        MSG_C2S_START_MATCH    = 14, // C→S  (空 payload)　仅房主、在房中
        MSG_S2C_MATCH_START    = 15, // S→C  [4B 玩家数 n]{[4B PlayerId][1B 名长][名字][1B 出生点序号]}×n
        MSG_C2S_MOVE_REPORT    = 16, // C→S  [12B pos xyz f32][4B yaw f32][4B pitch f32][1B flags]（bit0=腾空）
        MSG_S2C_ENTITY_STATE   = 17, // S→C v4.8 [2B 数量 n]{[4B PlayerId][12B pos][4B yaw][4B pitch][1B flags][4B ackedSeq]}×n（29B/实体·含接收者自己 v4.1；flags bit0 腾空/bit1 死亡/bit2 无敌/bit3~4 weaponType）
        MSG_S2C_ENTITY_LEAVE   = 18,
    MSG_C2S_PLAYER_INPUT    = 19, // [v4] 位置上报 v2：[4B seq][12B pos][4B yaw][4B pitch][1B flags]=25B——对局期替代 16（R10）
        // —— 第 5 期：开火与命中（与 docs/protocol/MSG_IDS.md v4.2 保持同步）——
        MSG_C2S_FIRE            = 20, // C→S  (空 payload)——判定全用服务器权威状态，方向都不收（D5-01）
        MSG_S2C_FIRE_RESULT     = 21, // S→C v4.8.1 [4B shooterPid][1B result][4B victimPid][12B hitPoint][1B victimHp][1B gate][2B reloadRemainingMs]=25B——房内全员广播（含射手）
        MSG_C2S_RTT_REPORT      = 22, // C→S  [2B rttMs][2B upKB/s][2B downKB/s]=6B·1Hz——ep06 回溯量源+带宽计数预埋（ep12 消费）
        // —— 第 7 期：死亡/重生/结算 + 人机席位（v4.4）——
        MSG_S2C_KILL            = 23, // S→C  [4B victimPid][4B killerPid][2B respawnDelayMs]=10B——房内全员广播（击杀播报+比分源）
        MSG_S2C_RESPAWN         = 24, // S→C v4.5 [4B pid][1B spawnIndex][2B invulnMs][4B x][4B y][4B z][4B yaw]=23B——服务器权威落位（不再查表）
        MSG_S2C_MATCH_END       = 25, // S→C  [1B 原因][1B n]{[4B pid][2B kills][2B deaths]}×8n+2——对局结束+结算比分
        MSG_C2S_BOT_ADD         = 26, // C→S  [1B 难度]——仅房主（满 4 人拒绝）
        MSG_C2S_BOT_REMOVE      = 27, // C→S  [4B botPid]——仅房主
        // —— 第 8 期：契约 v4.8（已实施）——
        MSG_C2S_SWITCH_WEAPON   = 28, // C→S  [1B weaponType]（0=Rifle / 1=Pistol）——换枪意图，20 号维持空包
        MSG_C2S_RELOAD          = 29, // C→S  (空)——自主换弹
        // ===== ep09 v4.9：账号域（31~35·异步回执·与房级消息无顺序保证——域注记见 MSG_IDS.md）=====
        MSG_C2S_REGISTER        = 31, // C→S  [1B accountLen][account≤24B][1B passLen][pass≤64B]——成功即自动登录态
        MSG_C2S_ACCOUNT_LOGIN   = 32, // C→S  [1B accountLen][account≤24B][1B passLen][pass≤64B]——账号登录（4 号游客并行零改动）
        MSG_S2C_AUTH_RESULT     = 33, // S→C  [1B ok][1B reason][4B accountId][1B mode][1B nameLen][name]≥8B——accountId≠pid（E7）
        MSG_C2S_QUERY_STATS     = 34, // C→S  [1B limit≤20]——身份从连接上下文取（防伪造·只查自己）
        MSG_S2C_STATS_RESULT    = 35, // S→C  [1B count]{[4B matchId][1B kills][1B deaths][8B endedAt]}×count
        MSG_S2C_CONFIG_VERSION  = 30, // S→C  [1B verLen][ver][1B N]{[1B type][1B damage][1B mag][2B reloadMs][2B fireMs]}×N（每项 7B）
        // 9 号 JOIN_RESULT 码表 +5 = 对局进行中（对局中拒 JOIN）
    };

} // namespace protocol
