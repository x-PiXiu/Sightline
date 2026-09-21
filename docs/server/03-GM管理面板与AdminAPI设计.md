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
