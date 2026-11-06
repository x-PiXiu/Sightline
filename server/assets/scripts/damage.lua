-- damage.lua —— ep08 步骤 4：公式面（纯函数·patch 通道）
-- §4.6 C1 纪律：零 local 常量、零 os/io、零时钟——可调值一律从注入参数读：
--   base   ← ICombatParams::Weapon(type).damage（§3.2 武器表唯一真相源）
--   params ← ICombatParams::Formula()（game_config.lua formula 段）
--   ctx    ← C++ 构造 {dist, headshot}（headshot ep08 恒 false·G6 爆头球 ep10+ 兑现）
-- 热修：`patch damage` 走引用 swap（§4.5 upvalue 彩蛋的正解——换引用不改值）。
return function(hp, base, ctx, params)
    local dmg = base
    if ctx.headshot then dmg = dmg * params.headshot_multiplier end
    if ctx.dist > params.falloff_start_cm then dmg = dmg * params.falloff_factor end
    return math.max(0, hp - dmg)
end
