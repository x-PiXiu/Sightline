# Sightline

企业级缩小版多人 FPS：**自研 Linux C++ 服务器 + UE5 客户端**，开发全程视频公开记录。

> 一家不存在的公司，一份真实的工作。——《[领导请批示](https://space.bilibili.com/) · 开发周报》

## 系列总目录

| 期 | 视频 | tag | 新增功能 | KPI |
|----|------|-----|----------|-----|
| ep01 · 入职第一周：UE 连上服务器 | 待发布 | `ep01` | 心跳 / ping / UE 首连 | 待录 |

## 架构

- **服务端** `server/`：epoll 主从 Reactor，IO 多线程 + 逻辑单线程；整洁架构四层（domain / application / adapters / net）
- **客户端** `client/`：UE5，C++ 为主，蓝图只做表现层；表现层素材来自 Epic 官方 Lyra（见 `client/README.md` 依赖说明）

## 文档

- `docs/episodes/` 每期定稿架构图、接口定义、KPI 数据
- `docs/protocol/` 协议文档
- `docs/legacy/` 前任（毕业时的我）遗留：架构与协议文档、压测数据来源

## 构建

```bash
# server（Linux / WSL）
cd server && cmake -B build && cmake --build build -j
```
