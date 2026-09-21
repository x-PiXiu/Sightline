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

    -- GM 管理 API（docs/server/03）：port=0 关闭；token 为 Bearer 凭据
    -- ⚠️ token 为开发期默认值；正式部署必须更换（后续接环境变量注入）
    admin = {
        port  = 8080,
        token = "sightline-dev-token",
    },

    -- D2 起由存储模块消费（文档：docs/server/01）——扁平键（main.cpp getString("database", key) 直读）
    -- ⚠️ 凭据为开发期临时值；正式部署时改由环境变量注入，不入库不入 git
    database = {
        mysql_host     = "172.17.153.223",
        mysql_port     = 3306,
        mysql_user     = "root",
        mysql_password = "123456",
        mysql_database = "sightline",
        mysql_pool     = 4,
        redis_host     = "172.17.153.223",
        redis_port     = 6379,
        redis_password = "123456",
        mongo_uri      = "mongodb://root:123456@172.17.153.223:27017",
        mongo_database = "sightline",
    },
}
