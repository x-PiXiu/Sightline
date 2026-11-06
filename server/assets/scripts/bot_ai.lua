-- bot_ai.lua —— ep08 步骤 8：决策面（四态 FSM·纯函数·patch 通道）
-- 移植自 match/bot_brain.h BuiltinBotPolicy::Decide（等价性对拍=§4.7 第一层门禁）。
-- §4.6 C1 纪律：零可调 local 常量、零 os/io、零时钟——可调值一律从注入参数读：
--   tune ← BotTuning 十旋钮 + FSM 语义常量五件（值唯一真相源=bot_brain.h detail，桥注入）
--   mem / st ← C++ 传入并就地改写（跨 tick 记忆与状态机归 driver 持有，脚本无状态）
-- 状态序镜像 C++ BotState 枚举（0=Patrol 1=Engage 2=Search 3=TakeCover，非可调值）。
local PATROL, ENGAGE, SEARCH, TAKE_COVER = 0, 1, 2, 3

local function yaw_to(fx, fy, tx, ty)
    return math.atan(ty - fy, tx - fx) * 180 / math.pi
end

local function norm_deg(d)
    while d > 180 do d = d - 360 end
    while d < -180 do d = d + 360 end
    return d
end

return function(view, st, mem, nowMs, tune)
    local intent = { has_move = false, move_x = 0, move_y = 0, aim_yaw_deg = 0, fire = false }
    local self_ = view.self

    -- —— 全局感知落地：看见即记忆（遗忘在无视线分支处理）——
    if view.visible then
        -- 新交战判定：距上次可见超过遗忘窗（或首次）→ 重置收敛时钟与误差
        if mem.last_seen_ms == 0 or nowMs - mem.last_seen_ms > tune.forget_ms then
            mem.first_seen_ms = nowMs
            mem.aim_err_deg = tune.err_start_deg
        end
        mem.last_seen_ms = nowMs
        mem.lk_x = view.visible.x
        mem.lk_y = view.visible.y
        mem.has_last_known = true
    end

    -- —— 血量门：受击（血量较上次可见进一步下降且低于阈值）→ 短暂脱战 → 到期反击
    --     （2026-11-05 用户实锤"受击后一直往后退"：受击事件触发一个撤退窗，窗满无条件
    --      转 Engage/Search；重入需新的伤害事件）——
    if view.visible and self_.hp < tune.hp_cover_below and self_.hp < mem.last_hp_seen
       and nowMs >= mem.next_cover_ok_ms then
        mem.next_cover_ok_ms = nowMs + tune.cover_window_ms
        st.s = TAKE_COVER
    end
    if view.visible then mem.last_hp_seen = self_.hp end   -- 可见时记账（不可见期间的伤害无法归因）
    if st.s == TAKE_COVER and nowMs >= mem.next_cover_ok_ms then
        st.s = view.visible and ENGAGE or SEARCH           -- 窗满=反击（打到底，不再无限后撤）
    end

    if st.s == PATROL then
        if view.visible then st.s = ENGAGE                 -- 发现 → 交战（误差重置已由感知段处理）
        else
            intent.has_move = true
            intent.move_x = view.patrol.x
            intent.move_y = view.patrol.y
            intent.aim_yaw_deg = yaw_to(self_.x, self_.y, view.patrol.x, view.patrol.y)
        end

    elseif st.s == ENGAGE then
        if not view.visible then                           -- 丢视线：窗内去最后已知位置找
            if mem.has_last_known and nowMs - mem.last_seen_ms <= tune.forget_ms then
                st.s = SEARCH
            else
                st.s = PATROL
                mem.aim_err_deg = 0
            end
        else
            local e = view.visible
            local dist = e.dist
            -- 瞄准：目标方向 + 收敛误差（误差随交战时长衰减）
            local engaged_s = (nowMs - mem.first_seen_ms) / 1000
            mem.aim_err_deg = math.max(tune.err_end_deg, tune.err_start_deg - tune.err_decay_deg_per_s * engaged_s)
            local base_yaw = yaw_to(self_.x, self_.y, e.x, e.y)
            local noise_sign = math.floor(nowMs / tune.jitter_period_ms) % 2 == 0 and 1 or -1  -- 慢抖动（非逐帧噪声）
            intent.aim_yaw_deg = base_yaw + mem.aim_err_deg * noise_sign
            -- 开火条件：对准（误差锥内）+ 射程内
            local aim_delta = math.abs(norm_deg(intent.aim_yaw_deg - self_.yaw))
            intent.fire = aim_delta <= tune.fire_cone_deg and dist <= tune.engage_range_cm and not self_.reloading
            -- 走位：超出交战距离 → 追近（沿视线方向直进——寻路由 driver 栅格兜底）
            if dist > tune.engage_range_cm then
                intent.has_move = true
                intent.move_x = e.x
                intent.move_y = e.y
            end
        end

    elseif st.s == SEARCH then
        if view.visible then st.s = ENGAGE
        elseif not mem.has_last_known or nowMs - mem.last_seen_ms > tune.forget_ms then
            st.s = PATROL                                  -- 遗忘 → 回巡逻
            mem.aim_err_deg = 0
        else
            intent.has_move = true
            intent.move_x = mem.lk_x
            intent.move_y = mem.lk_y
            intent.aim_yaw_deg = yaw_to(self_.x, self_.y, mem.lk_x, mem.lk_y)
            local dx = self_.x - mem.lk_x
            local dy = self_.y - mem.lk_y
            if dx * dx + dy * dy < tune.arrive_radius_cm * tune.arrive_radius_cm then
                st.s = PATROL                              -- 到达最后已知位仍未见 → 回巡逻
                mem.has_last_known = false
            end
        end

    else -- TAKE_COVER
        if nowMs >= mem.next_cover_ok_ms then              -- 窗满=无条件反击（打到底）
            st.s = view.visible and ENGAGE or SEARCH
        else
            -- 背向敌人撤退（有视线=背向；无视线=向记忆点反方向）
            local ex, ey = 0, 0
            if view.visible then
                ex = view.visible.x - self_.x
                ey = view.visible.y - self_.y
            elseif mem.has_last_known then
                ex = mem.lk_x - self_.x
                ey = mem.lk_y - self_.y
            end
            local len = math.sqrt(ex * ex + ey * ey)
            if len > 1e-3 then
                intent.has_move = true
                intent.move_x = self_.x - ex / len * tune.retreat_dist_cm
                intent.move_y = self_.y - ey / len * tune.retreat_dist_cm
            end
            if view.visible then
                intent.aim_yaw_deg = yaw_to(self_.x, self_.y, view.visible.x, view.visible.y)
            else
                intent.aim_yaw_deg = yaw_to(self_.x, self_.y, self_.x + ex, self_.y + ey)
            end
        end
    end

    return intent
end
