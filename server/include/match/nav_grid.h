// nav_grid.h —— ep07 导航栅格（域层纯函数·零依赖·脱离网络纯单测）
// 设计（ep07 设计草案 §4.3·D7-05）：遮挡盒 → 栅格化 → A* —— 数据单一真相源 = occluders
// （栅格为启动期派生数据，不入地图 JSON——防两份数据漂移）。
// 语义：①只有与「身体高度带」相交的盒才挡路（地板盒顶在脚下→自动排除；
//       台阶以下（kNavStepCm）可跨越）②盒按命中体半径膨胀（闵可夫斯基和近似=对盒尺寸加半径）
//       ③栅格点=格中心；blocked 格不可站立。地图边界：±kNavMapHalf（4000² 地图+100 余量）。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "match/fire_judge.h"   // OccluderTable / Vec3 / 常量（依赖向内）

namespace sightline::match {

struct NavGrid {
    int w = 0, h = 0;                       // 格数（X/Y）
    float cellCm = kNavCellCm;
    float originX = -2100.f, originY = -2100.f;   // 地图左下角（4000² 地图 + 100 余量）
    std::vector<std::uint8_t> blocked;      // 1=不可站立

    bool InBounds(int ix, int iy) const { return ix >= 0 && iy >= 0 && ix < w && iy < h; }
    bool Blocked(int ix, int iy) const { return !InBounds(ix, iy) || blocked[static_cast<std::size_t>(iy) * w + ix] != 0; }
    // 格中心世界坐标
    float CellX(int ix) const { return originX + (ix + 0.5f) * cellCm; }
    float CellY(int iy) const { return originY + (iy + 0.5f) * cellCm; }
    // 世界坐标 → 格下标（越界钳边）
    int CellIx(float x) const { return std::clamp(static_cast<int>((x - originX) / cellCm), 0, w - 1); }
    int CellIy(float y) const { return std::clamp(static_cast<int>((y - originY) / cellCm), 0, h - 1); }
};

/** 栅格化：盒与「身体高度带」相交（盒顶>台阶 且 盒底<身高顶）→ 按半径膨胀 → 落格。
 *  地板盒（顶面在脚下）天然排除——不做任何特判。纯函数：同输入同栅格。 */
inline NavGrid BuildNavGrid(const OccluderTable& occ) {
    NavGrid g;
    const float mapHalf = 2100.f;
    g.originX = -mapHalf; g.originY = -mapHalf;
    g.w = g.h = static_cast<int>(2 * mapHalf / kNavCellCm);   // 84×84
    g.blocked.assign(static_cast<std::size_t>(g.w) * g.h, 0);
    const float inflate = kNavInflateCm;
    for (const auto& b : occ.boxes) {
        const float bot = b.cz - b.hz, top = b.cz + b.hz;
        if (!(top > kNavStepCm) || !(bot < kNavBodyTopCm)) continue;   // 不在身体高度带：不挡路
        // OBB 足印（绕 Z）→ 外接 AABB + 膨胀半径（保守：转角处多封几格，安全侧）
        const float ry = b.yawDeg * kPi / 180.f;
        const float c = std::fabs(std::cos(ry)), s = std::fabs(std::sin(ry));
        const float ex = (c * b.hx + s * b.hy) + inflate;   // 旋转后 AABB 半宽
        const float ey = (s * b.hx + c * b.hy) + inflate;   // 旋转后 AABB 半高
        const int ix0 = g.CellIx(b.cx - ex), ix1 = g.CellIx(b.cx + ex);
        const int iy0 = g.CellIy(b.cy - ey), iy1 = g.CellIy(b.cy + ey);
        for (int iy = iy0; iy <= iy1; ++iy)
            for (int ix = ix0; ix <= ix1; ++ix)
                if (g.InBounds(ix, iy)) g.blocked[static_cast<std::size_t>(iy) * g.w + ix] = 1;
    }
    return g;
}

/** 栅格 LOS（走格判定：Bresenham 直线过 blocked 格=不可视/不可直行） */
inline bool GridLOS(const NavGrid& g, int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        if (g.Blocked(x0, y0)) return false;
        if (x0 == x1 && y0 == y1) return true;
        const int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/** A*（8 邻域·对角代价 √2）+ LOS 拉直——返回世界坐标路径（含起终点；不可达=nullopt）。 */
inline std::optional<std::vector<Vec3>> FindPath(const NavGrid& g, float fromX, float fromY,
                                                 float toX, float toY) {
    const int sx = g.CellIx(fromX), sy = g.CellIy(fromY);
    const int tx = g.CellIx(toX),   ty = g.CellIy(toY);
    if (g.Blocked(tx, ty)) return std::nullopt;
    if (sx == tx && sy == ty) return std::vector<Vec3>{{toX, toY, 0.f}};
    const int n = g.w * g.h;
    std::vector<float> gScore(static_cast<std::size_t>(n), std::numeric_limits<float>::infinity());
    std::vector<int> cameFrom(static_cast<std::size_t>(n), -1);
    std::vector<std::uint8_t> closed(static_cast<std::size_t>(n), 0);
    const auto idx = [&](int ix, int iy) { return iy * g.w + ix; };
    const auto hEst = [&](int ix, int iy) {
        const float dx = std::abs(ix - tx), dy = std::abs(iy - ty);
        return (dx + dy) + (1.41421356f - 2.f) * std::min(dx, dy);   // 八向启发
    };
    // 小根堆（pair: f, idx）
    std::vector<std::pair<float, int>> open;
    auto pushOpen = [&](float f, int i) {
        open.push_back({f, i});
        std::push_heap(open.begin(), open.end(),
                       [](const auto& a, const auto& b) { return a.first > b.first; });
    };
    auto popOpen = [&]() {
        std::pop_heap(open.begin(), open.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
        int i = open.back().second;
        open.pop_back();
        return i;
    };
    gScore[idx(sx, sy)] = 0.f;
    pushOpen(hEst(sx, sy), idx(sx, sy));
    static constexpr int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    static constexpr float COST[8] = {1, 1, 1, 1, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f};
    int goalIdx = idx(tx, ty);
    while (!open.empty()) {
        const int cur = popOpen();
        if (closed[cur]) continue;
        closed[cur] = 1;
        if (cur == goalIdx) break;
        const int cx = cur % g.w, cy = cur / g.w;
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + DX[k], ny = cy + DY[k];
            if (!g.InBounds(nx, ny) || g.Blocked(nx, ny)) continue;
            if (DX[k] != 0 && DY[k] != 0 &&
                (g.Blocked(cx + DX[k], cy) || g.Blocked(cx, cy + DY[k])))
                continue;                                        // 对角穿越缝隙禁止（防贴角穿模）
            const float ng = gScore[cur] + COST[k];
            const int ni = idx(nx, ny);
            if (ng < gScore[ni]) {
                gScore[ni] = ng;
                cameFrom[ni] = cur;
                pushOpen(ng + hEst(nx, ny), ni);
            }
        }
    }
    if (gScore[goalIdx] == std::numeric_limits<float>::infinity()) return std::nullopt;
    // 回溯
    std::vector<Vec3> path;
    for (int cur = goalIdx; cur != -1; cur = cameFrom[cur])
        path.push_back({g.CellX(cur % g.w), g.CellY(cur / g.w), 0.f});
    std::reverse(path.begin(), path.end());
    // LOS 拉直（贪心跳点：能直视就跳过中间格）
    std::vector<Vec3> smooth;
    std::size_t i = 0;
    while (i < path.size()) {
        std::size_t j = path.size() - 1;
        while (j > i + 1 &&
               !GridLOS(g, g.CellIx(path[i].x), g.CellIy(path[i].y), g.CellIx(path[j].x), g.CellIy(path[j].y)))
            --j;
        if (j <= i) j = i + 1;                       // 死循环防御（j==i 时强制推进——理论不可达，
        smooth.push_back(path[j]);                    //  实际=GridLOS 在相邻格间返回 false 的边界）
        i = j;
    }
    smooth.back() = {toX, toY, 0.f};                            // 终点贴到精确请求位
    return smooth;
}

} // namespace sightline::match
