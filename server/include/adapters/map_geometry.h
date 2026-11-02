// map_geometry.h —— 第 5 期几何合同装载（适配层：import nlohmann-json——域层 fire_judge.h 对此一无所知）
// 合同 v1（server/assets/maps/*.json）：编辑器烘焙产物（蓝图方案 M5-F Srv.BakeOccluders）——非运行时上报（D5-08v2）
// 结构：{ map, unit:"cm", occluders:[{primitives:[{type:box|sphere, pos, size/r, yawDeg?}], fallbackAABB?}],
//         entityHitbox:{radius, halfHeight} }
// 语义：box 的 size=半尺寸、yawDeg=绕 Z 旋转（缺省 0=AABB）；sphere 的 size 语义=r；原语缺失时用 fallbackAABB 兜底
// 校验：occluders ≤64（kMaxOccluders）· 半尺寸/半径>0 · 数值有限——非法即 nullopt（调用方 fail-fast）
#pragma once
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "match/fire_judge.h"

namespace adapters {

/** 核心：解析合同文本 → 域层几何（与文件读取分离——纯单测可达，整洁架构体检第 3 条） */
inline std::optional<sightline::match::MapGeometry> ParseMapGeometry(std::string_view text) {
    namespace json = nlohmann;
    sightline::match::MapGeometry out;
    json::json root;
    try {
        root = json::json::parse(text);
    } catch (...) {
        return std::nullopt;                                 // 非法 JSON：装配错误，fail-fast 由调用方执行
    }
    if (!root.is_object()) return std::nullopt;
    if (root.contains("unit") && root["unit"] != "cm") return std::nullopt;   // 单位合同：只认厘米

    const auto finite3 = [](const json::json& a) {
        return a.is_array() && a.size() == 3 && a[0].is_number() && a[1].is_number() && a[2].is_number();
    };

    if (root.contains("entityHitbox")) {
        const auto& hb = root["entityHitbox"];
        if (!hb.is_object()) return std::nullopt;
        if (hb.contains("radius") && hb["radius"].is_number())
            out.hitbox.radius = hb["radius"].get<float>();
        if (hb.contains("halfHeight") && hb["halfHeight"].is_number())
            out.hitbox.halfHeight = hb["halfHeight"].get<float>();
        if (!(out.hitbox.radius > 0.f) || !(out.hitbox.halfHeight > 0.f)) return std::nullopt;
    }

    if (root.contains("occluders")) {
        const auto& occ = root["occluders"];
        if (!occ.is_array()) return std::nullopt;
        if (occ.size() > sightline::match::kMaxOccluders) return std::nullopt;
        for (const auto& item : occ) {
            if (!item.is_object() || !item.contains("primitives") || !item["primitives"].is_array())
                return std::nullopt;
            bool hasPrimitive = false;
            for (const auto& p : item["primitives"]) {
                if (!p.is_object() || !p.contains("type") || !p["type"].is_string()) return std::nullopt;
                const std::string type = p["type"].get<std::string>();
                if (type == "box") {
                    if (!p.contains("pos") || !finite3(p["pos"]) || !p.contains("size") || !finite3(p["size"]))
                        return std::nullopt;
                    sightline::match::OccluderBox b;
                    b.cx = p["pos"][0].get<float>(); b.cy = p["pos"][1].get<float>(); b.cz = p["pos"][2].get<float>();
                    b.hx = p["size"][0].get<float>(); b.hy = p["size"][1].get<float>(); b.hz = p["size"][2].get<float>();
                    if (p.contains("yawDeg") && p["yawDeg"].is_number()) b.yawDeg = p["yawDeg"].get<float>();
                    if (!(b.hx > 0.f) || !(b.hy > 0.f) || !(b.hz > 0.f)) return std::nullopt;
                    if (!std::isfinite(b.cx) || !std::isfinite(b.cy) || !std::isfinite(b.cz)) return std::nullopt;
                    out.occluders.boxes.push_back(b);
                    hasPrimitive = true;
                } else if (type == "sphere") {
                    if (!p.contains("pos") || !finite3(p["pos"]) || !p.contains("r") || !p["r"].is_number())
                        return std::nullopt;
                    sightline::match::OccluderSphere s;
                    s.cx = p["pos"][0].get<float>(); s.cy = p["pos"][1].get<float>(); s.cz = p["pos"][2].get<float>();
                    s.r = p["r"].get<float>();
                    if (!(s.r > 0.f) || !std::isfinite(s.cx) || !std::isfinite(s.cy) || !std::isfinite(s.cz))
                        return std::nullopt;
                    out.occluders.spheres.push_back(s);
                    hasPrimitive = true;
                } else {
                    return std::nullopt;                     // 未知原语类型：合同不认识即拒（fail-fast）
                }
            }
            if (!hasPrimitive && item.contains("fallbackAABB")) {   // 原语为空时兜底（保留通道）
                const auto& fb = item["fallbackAABB"];
                if (!fb.is_object() || !fb.contains("center") || !finite3(fb["center"]) ||
                    !fb.contains("half") || !finite3(fb["half"]))
                    return std::nullopt;
                sightline::match::OccluderBox b;
                b.cx = fb["center"][0].get<float>(); b.cy = fb["center"][1].get<float>(); b.cz = fb["center"][2].get<float>();
                b.hx = fb["half"][0].get<float>(); b.hy = fb["half"][1].get<float>(); b.hz = fb["half"][2].get<float>();
                if (!(b.hx > 0.f) || !(b.hy > 0.f) || !(b.hz > 0.f)) return std::nullopt;
                out.occluders.boxes.push_back(b);
            }
        }
    }
    // —— ep07 地图合同 v2：spawnPoints/patrolPoints（可选字段——缺省=空表降级，调用方回退场心）——
    if (root.contains("spawnPoints")) {
        const auto& sp = root["spawnPoints"];
        if (!sp.is_array()) return std::nullopt;
        for (const auto& pt : sp) {
            if (!pt.is_object() || !pt.contains("pos") || !finite3(pt["pos"])) return std::nullopt;
            sightline::match::SpawnPoint s;
            s.x = pt["pos"][0].get<float>(); s.y = pt["pos"][1].get<float>(); s.z = pt["pos"][2].get<float>();
            if (pt.contains("yawDeg") && pt["yawDeg"].is_number()) s.yawDeg = pt["yawDeg"].get<float>();
            out.spawnPoints.push_back(s);
        }
    }
    if (root.contains("patrolPoints")) {
        const auto& pp = root["patrolPoints"];
        if (!pp.is_array()) return std::nullopt;
        for (const auto& pt : pp) {
            if (!pt.is_object() || !pt.contains("pos") || !finite3(pt["pos"])) return std::nullopt;
            out.patrolPoints.push_back({pt["pos"][0].get<float>(), pt["pos"][1].get<float>(), pt["pos"][2].get<float>()});
        }
    }
    // "nav" 参数（cellCm/stepCm 等）本期住 nav_grid.h 常量——JSON 可带但忽略（向前兼容占位）
    return out;
}

/** 文件读取包装：读不到/解析失败 → nullopt（调用方决定 fail-fast 还是空表兼容） */
inline std::optional<sightline::match::MapGeometry> LoadMapGeometry(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ParseMapGeometry(ss.str());
}

} // namespace adapters
