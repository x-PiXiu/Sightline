# Sightline 协议登记表（服务端与 UE 的共同契约）

> 传输：TCP 流。帧格式：`[2B len][2B msgid][payload]`，**小端**。
> `len` = msgid(2B) + payload 的总字节数（不含 len 自身）。
> 任何一端改动本表 = 契约变更，必须同步另一端并在本文件记录变更历史。

## 消息枚举

| msgid | 名称 | 方向 | payload 结构 | 说明 |
|-------|------|------|--------------|------|
| 1 | MSG_S2C_WELCOME | S → C | `[8B serverNowMs]` | 连接建立后服务器立即发送；C 可据此校时（第 1 期仅作欢迎） |
| 2 | MSG_C2S_HEARTBEAT | C → S | `[4B seq][8B clientTs]` | C 每 1s 发送；seq 从 1 递增；clientTs = C 当前毫秒时戳 |
| 3 | MSG_S2C_HEARTBEAT_ACK | S → C | `[4B seq][8B clientTs][8B serverTs]` | S 原样回显 seq/clientTs + 附加 serverTs；C 按 seq 匹配后 `ping = now − clientTs` |
| 4+ | （第 2 期起：LOGIN / ROOM_LIST / …） | | | 新消息必须先在本表登记再写代码 |

## 变更历史

| 日期 | 变更 | 提交 |
|------|------|------|
| 2026-10-02 | 初版：WELCOME / HEARTBEAT / HEARTBEAT_ACK 三条 | 第 1 期 |
