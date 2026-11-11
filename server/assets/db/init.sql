-- init.sql —— ep08 步骤 5：MySQL DDL（真相库·must）
-- 重放安全（IF NOT EXISTS）；时间一律 UTC epoch ms（§3.4 双钟分立：审计=墙钟，游戏=steady）。
-- 执行：mysql -u root < assets/db/init.sql（或 db_up.sh 自动执行）

CREATE TABLE IF NOT EXISTS t_config_version (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    version     VARCHAR(32)  NOT NULL,               -- v7-a3f2c1d8
    source      VARCHAR(32)  NOT NULL,               -- console@reload / rollback / script_patch
    diff_digest VARCHAR(64)  NOT NULL DEFAULT '',    -- FNV-1a 8 hex（§3.4）
    note        VARCHAR(255) NOT NULL DEFAULT '',    -- version_note（操作人动机）
    created_at  BIGINT       NOT NULL                -- UTC epoch ms
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 战绩聚合行（Ended 落库·B3 修复带 config_version）：一局一人一行
CREATE TABLE IF NOT EXISTS t_account (
  id         INT AUTO_INCREMENT PRIMARY KEY,
  name       VARCHAR(24) NOT NULL UNIQUE,
  pass_hash  VARCHAR(128) NOT NULL,
  created_at BIGINT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

ALTER TABLE t_match_result ADD COLUMN IF NOT EXISTS account_id INT NULL;   -- 游客=NULL；注册回溯绑定不做（边界）

CREATE TABLE IF NOT EXISTS t_match_result (
    match_id      VARCHAR(40) NOT NULL,              -- 20261106T023015Z-003（C3：UTC 戳+会话序号）
    player_name   VARCHAR(64) NOT NULL,
    kills         INT         NOT NULL DEFAULT 0,
    deaths        INT         NOT NULL DEFAULT 0,
    ended_at      BIGINT      NOT NULL,              -- UTC epoch ms
    config_version VARCHAR(32) NOT NULL DEFAULT '',  -- 该局生效配置（审计链三连之一）
    PRIMARY KEY (match_id, player_name),
    KEY idx_ended_at (ended_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
