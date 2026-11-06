-- game_config.lua —— ep08 纯数据表（⚠ 纪律：只放数字，不放逻辑）
-- 加载路径：config_service.cpp 解析 → C++ 逐字段校验（含跨字段）→ GameConfig → 双缓冲
-- ⚠ 单一真相源：武器数值只在 weapons[type] 出现一次；combat 镜像已删除（改了不生效的坑）。
-- 运营改表后敲 `reload [note]`，服务器不重启、下一发即时生效；`rollback` 内存回退上一版。
return {
    rules      = { max_hp = 100 },                  -- 非武器的全局规则
    deathmatch = { respawn_ms = 3000, invuln_ms = 2000, kill_limit = 10,
                   spawn_clear_radius_cm = 150 },
    rewind     = { max_rewind_ms = 1000 },
    bot        = { react_ms = 250, err_start_deg = 4, err_end_deg = 1,
                   err_decay_deg_per_s = 3, turn_rate_deg_s = 360,
                   forget_ms = 3000, engage_range_cm = 900,
                   hp_cover_below = 30, move_speed_cm_s = 400,
                   fire_interval_ms = 400 },
    -- ★ 武器数值唯一真相源（key = 契约锁 EWeaponType 的字符串别名）
    weapons    = { rifle  = { damage = 25, mag = 30, reload_ms = 2500, fire_ms = 95 },
                   pistol = { damage = 20, mag = 12, reload_ms = 1800, fire_ms = 220 } },
    formula    = { headshot_multiplier = 2.0, falloff_start_cm = 3000,
                   falloff_factor = 0.8 },
}
