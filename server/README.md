# server/

**Sightline 服务端**。代码基线：**毕设网络库原版**（MicroserviceDemo common/network + logger + 依赖闭包），原样迁入、未做修改——后续视频叙事中为"前任员工（毕业时的我）留下的网络库遗产"。

## 目录

```
include/common/          # 毕设原码头文件（network / logger / thread_pool / config）
src/common/              # 毕设原版实现
tests/smoke_base.cpp     # 底座冒烟测试（EventLoop 创建 → 定时器 → 干净退出）
```

## 构建（WSL / Linux）

```bash
cmake -B build && cmake --build build -j
./build/smoke_base        # 底座冒烟：PASS = 遗产活着
```

依赖：g++ >= 13、cmake >= 3.16、yaml-cpp、nlohmann-json（config_manager 原版依赖）。

## 第 1 期待开发

设计图定稿（docs/episodes/ep01/）后实施：Acceptor / ClientConnection / main（心跳 echo）——见《每期设计图规范》映射表。
