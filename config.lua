-- sightline 服务器配置（热更单一来源）
-- 生效方式：
--   network.port            → 重启生效（监听器不可热换；命令行参数优先级更高）
--   network.heartbeat_*     → reload 热更生效
--   game.*                  → reload 热更生效（进行中对局不受影响，新开局生效）
--   database.*              → D2 起被存储模块读取（连接池地址变更需重启）
-- 管理命令（控制台）：reload = 热载本文件；quit = 退出

return {
    network = {
        port                = 8888,
        heartbeat_timeout_ms = 8000,   -- 开发期曾临时 15000，自热更体系上线回归 8000
        scan_interval_ms    = 1000,
    },

    game = {
        win_kills   = 3,               -- 热更演示：改为 5 → 控制台 reload → 5 杀判胜
        respawn_ms  = 3000,
        max_players = 8,
    },

    -- D2 起由存储模块消费（文档：docs/server/01）
    -- ⚠️ 凭据为开发期临时值；正式部署时改由环境变量注入，不入库不入 git
    database = {
        mysql = { host = "172.17.153.223", port = 3306, user = "root",
                  password = "123456", database = "sightline", pool_size = 4 },
        redis = { host = "172.17.153.223", port = 6379, password = "123456" },
        mongo = { uri = "mongodb://root:123456@172.17.153.223:27017", database = "sightline" },
    },
}
