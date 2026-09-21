# 03 · GM 管理面板与 Admin API 设计

> **定位**：GM（Game Master）工具的技术选型与实现方案——Web 管理面板 + HTTP Admin API。
> 对应实施排期约**半天**（AdminHttpServer + AdminHandler + GM 面板 HTML + main 整合）。
>
> **前置**：01 号存储与脚本基础设施（StorageIO/三驱动/LuaVM）已落地；
> 02 号业务逻辑（账号/战绩/房间）已落地。GM 工具是这两层的**第二个消费者**。

---

## 一、企业 GM 工具的三种形态

| 形态 | 交互方式 | 典型场景 | 优缺点 |
|---|---|---|---|
| **Web 管理面板**（最主流） | 浏览器 → HTTP REST API → 游戏服务器 | 运营后台、客服工具、数据看板 | ✅ 跨平台/无需安装/权限体系成熟 |
| 游戏内命令 | 聊天框输入 `/give 100` | 测试调试、线上紧急处理 | ✅ 零成本 / ❌ 功能受限 |
| 独立桌面程序 | 自定义协议 → TCP 直连 | 老项目、对实时性要求极高的场景 | ❌ 维护成本高，已过时 |

**企业标准答案：Web 管理面板 + HTTP Admin API**——浏览器打开一个页面，所有 GM 操作通过 HTTP 请求打到游戏服务器的管理端口，服务端执行后返回 JSON。

---

## 二、为什么 Web 面板 + HTTP Admin API 是正确答案

核心思想：**GM 工具是服务端的第二个客户端**——游戏客户端走 TCP 游戏协议，GM 工具走 HTTP 管理协议，两者调用同一套 application 层服务。

```
┌───────────── game_server 进程 ─────────────────────┐
│                                                    │
│  TCP :8888（游戏协议）    HTTP :8080（管理协议）       │
│      ▼                        ▼                    │
│  GameServer               AdminHttpServer           │
│      ▼                        ▼                    │
│  ┌────────── application 层（共用）──────────┐      │
│  │ AccountService / RoomService / ItemService │      │
│  └──────────────────────────────────────────┘      │
│      ▼                        ▼                    │
│  StorageIO 线程 ──▶ MySQL / Redis / Mongo           │
└────────────────────────────────────────────────────┘
```

**这就是整洁架构的直接收益**：application 层写的 AccountService/RoomService，游戏客户端在用，GM 工具也在用——同一套业务逻辑，两个入口，零重复代码。

---

## 三、GM 工具功能清单（企业标准 → Sightline 简易版）

| 功能 | 企业做法 | Sightline 简易版 | 优先级 |
|---|---|---|---|
| **服务器状态** | CPU/内存/连接数/TPS 实时图表 | GET /api/stats → JSON（已有 logStats 数据） | P0 |
| **玩家管理** | 搜索/封禁/踢出/查看详情 | GET /api/players、POST /api/kick | P0 |
| **房间管理** | 房间列表/强制关闭/查看状态 | GET /api/rooms、POST /api/close-room | P0 |
| **配置热更** | 修改数值 → 热生效 | 已有 reload 命令，补 HTTP 入口 | P0 |
| **排行榜管理** | 查看/手动调整/重置 | GET /api/top-kills | P1 |
| **发放道具** | 给指定玩家发道具/货币 | POST /api/give-item | P1 |
| **公告** | 全服广播文字公告 | POST /api/announce → 转发 GameEvent | P2 |
| **日志查询** | 操作审计日志 | 查 logger 文件 | P2 |

---

## 四、Sightline 实现方案

### 4.1 架构

在 game_server 进程里新增一个 **AdminHttpServer**（独立端口 8080），复用现有 epoll Reactor 的事件循环。HTTP 解析极简（只处理 GET/POST + JSON body），不需要引入完整 HTTP 库。

```
浏览器 GM 面板（HTML+JS）
    ▼ fetch("/api/stats")
HTTP :8080
    ▼ 解析请求 → 路由到对应方法
AdminHandler
    ▼ 调用 application 层（同一套 Service）
RoomService / AccountService / StorageIO
    ▼ 返回 JSON
```

### 4.2 新增文件

```
include/adapters/admin_http_server.h    HTTP 监听 + 路由分发
include/adapters/admin_handler.h        GM 操作 → 调用 application 层
www/admin/index.html                    GM 面板页面（纯 HTML+JS，无框架）
```

### 4.3 HTTP Admin API 设计

```
GET  /api/stats           → { conns, rooms, msgs_in, msgs_out, uptime_sec }
GET  /api/players         → [{ pid, name, account_id, room_id, kills, deaths }]
GET  /api/rooms           → [{ room_id, mode, state, cur, max }]
POST /api/kick            → { player_id }  → { ok }
POST /api/reload-config   → { ok }         ← 等价于控制台 reload
GET  /api/top-kills       → [{ account_id, kills }]
```

### 4.4 GM 面板页面（单文件 HTML，无框架依赖）

一个纯 HTML+JS 页面，用 `fetch()` 调 Admin API，结果渲染到表格。无 Node/无 npm/无框架——浏览器打开即用。

---

## 五、实施排期

| 步骤 | 内容 | 成本 |
|---|---|---|
| 1 | AdminHttpServer（HTTP 解析 + 路由）——复用 epoll Reactor | ~2 小时 |
| 2 | AdminHandler（状态/踢人/房间列表/热更 四个 API） | ~1 小时 |
| 3 | GM 面板 HTML（单文件，fetch 调 API） | ~1 小时 |
| 4 | 整合到 main.cpp（8080 端口启动） | ~15 分钟 |

**总计约半天**。这是把 application 层的"第二消费者"从蓝图扩展到 Web 的关键一步——以后所有运营需求（公告/封禁/数据看板）都往 AdminHandler 里加方法即可，游戏协议和 GM 面板互不干扰。

---

## 六、面试叙事

1. **"GM 工具怎么和游戏服务器交互？"**——答：HTTP Admin API 挂在游戏进程的管理端口上，Web 面板通过 REST 调用，和游戏客户端走 TCP 游戏协议共用同一套 application 层服务。拆分干净：游戏协议管实时对战，Admin API 管运营操作。
2. **"为什么不直连数据库？"**——直连 DB 绕过业务规则（缓存不一致/权限绕过/并发冲突）。GM 操作必须走 application 层服务，和玩家操作走同一条路。
3. **"如果规模大了怎么办？"**——Admin HTTP 独立成进程（运营平台），通过消息队列下发指令到各游戏服。当前端口边界已隔离，拆分只动 adapter。

---

*维护记录*

| 日期 | 修订 |
|---|---|
| 2026-09-21 | 初版：三种形态对比、Web+HTTP Admin API 选型、功能清单（企业标准→Sightline 简易版）、架构图、API 设计、实施排期、面试叙事 |

---

## 七、实现记录（2026-09-22，P0 全量落地）

### 7.1 交付清单

| 组件 | 文件 | 说明 |
|---|---|---|
| HTTP 服务 | include/adapters/admin_http_server.h | 独立阻塞线程 + 极简 HTTP/1.1 解析（请求行/Content-Length/Authorization），8KB 头部上限 + 64KB body 上限防恶意内存，收发 5s SO_*TIMEO 防半开连接挂死 |
| 路由+鉴权 | include/adapters/admin_api.h | Bearer token 鉴权；promise/future + queueInLoop 同步跳主 loop——HTTP 线程不碰游戏状态，逻辑保持单线程零锁；2s 超时兜底主 loop 卡死 |
| 面板 | www/admin/index.html | 单文件 HTML+JS（无框架），token 存 localStorage，状态/玩家/房间/排行四卡片 + 踢人/热更按钮，5s 自动刷新；由 Admin API 同源伺服（GET /），免 CORS |
| 装配 | main.cpp | reloadConfigFn 提为控制台/Admin API 共用；adminHttp.stop() 退场先于 storageIO |
| 配置 | config.lua [admin] 段 | port（0=关闭）+ token（Bearer 凭据，正式部署必须更换） |
| 端到端 | scripts/smoke_admin.py | 双客户端登录→自动匹配→Admin 核对同房→踢人核对离线 全链路冒烟 |

### 7.2 与设计稿的偏差（含依据）

1. **HTTP 不复用游戏 epoll Reactor（设计稿步骤 1 原计划复用）**——管理请求不可信且低频，解析崩溃/慢连接不能连累游戏 EventLoop；改独立阻塞线程，串行处理一两个浏览器绰绰有余。
2. **鉴权为固定 Bearer token**——设计稿漏了认证（缺口补丁）。面板页本身免 token（否则浏览器无法加载页面），token 由页面输入经 fetch 头携带；token 变更需重启（同端口语义）。
3. **/api/players 不含 kills/deaths**——成绩真源在 domain::Room::scoreBoard，MVP 先以房间维度观察；需要时按 roomIdOf 同款姿势扩展。
4. **top-kills 在主 loop 同步查 Redis**——管理查询低频 + 内网 RTT 亚毫秒可接受；高频化应改存储线程+回投（P1 演进项）。

### 7.3 顺带排掉的三颗雷（冒烟测试的直接产出）

1. **redis_conn.h 无限递归 + 空上下文解引用（致命，D4 潜伏）**：command() 与 ensureConnected() 互调 PING 构成无限递归（Redis 可连时栈溢出）；Redis 不可达时向 redisvCommand 传空上下文段错误。排行榜查询有 C2S 入口——等于客户端一个查询可打挂服务器。修复：PING 直接走 redisCommand 不经 command()；command() 连不上返回空 reply 降级。
2. **handleJoin 丢失 findJoinableRoom（致命，D4 提交 01c6ab1 回归）**：结构修复时误删自动匹配行，每次加入都开新房，双人永远凑不齐一局（应用层单测 test_room_service 拦截；HEAD 基线复现确认非新引入）。修复：恢复 `if (!room) room = findJoinableRoom();`。
3. **AdminHttpServer::stop() 死锁（本文件初版缺陷）**：Linux 上 close() 监听 fd 不会唤醒阻塞在 accept() 的线程，主线程 join 死等、进程退不干净。修复：先 shutdown(fd, SHUT_RDWR) 唤醒 accept 再 join（worker 若在 recv() 中最多等 5s 超时）。

### 7.4 验证记录

- 单测 7/7：hitscan / room / room_service / logger / lua / storage / account_repo 全绿
- curl 冒烟 11 端点：鉴权 401×2 / stats / players / rooms / top-kills / kick 离线错误 / reload / 面板 200 / 未知路由 404 / 存活复核 全过
- 端到端（smoke_admin.py）：双人自动匹配同房开战 → Admin 观察一致 → Admin 踢人（被踢方收 Kick 通告+断线，另一人保留）→ 进程优雅退出+端口释放

### 7.5 配置热更面板化（2026-09-22 补充）

GM 面板从"只能重载文件"升级为"在线改参数并热更"：

- **GET /api/config** → 当前生效的可调参数快照（5 个数值键）。
- **POST /api/config** `{"updates":{"game.win_kills":5}}` → 写回 config.lua + 立即热载，返回 `{"ok":true}`。
- **可热调白名单**（main 作为组合根持有）：`game.win_kills / respawn_ms / max_players`、`network.heartbeat_timeout_ms / scan_interval_ms`。database/admin 段需重启，不开放。
- **Lua 文本改写器**（main 匿名命名空间）：逐行只替换命中键的"值 token"——行内 `-- 注释` 与整体结构原样保留；全部键命中才写盘（hits 不齐拒绝半写）。
- **语义与控制台 reload 一致**：进行中对局不受影响，新开局生效；非法键 400 拒绝。
- 依赖方向不变：AdminApi 只做 JSON↔键值对翻译，读写在 main 注入的 ConfigGetter/ConfigSetter 回调；application/domain 零改动。

面板侧：新增"配置热更"卡片，输入框只按需读取（不随 5s 自动刷新，避免覆盖正在输入的值）；保存后回读服务端校验后的最终值；原"热更配置"按钮改名"从文件重载"（手动编辑文件后的磁盘→内存重载）。
