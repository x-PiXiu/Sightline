#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""parse_kpi06.py —— ep06 延迟补偿 KPI：命中率自动分桶统计（回溯关/开对比，反瞎编）

用法：
  python3 parse_kpi06.py --server <服务器.log> [--shooter <PlayerId>]

原理（单变量口径）：
  服务器 [射击判定] 行自带 回溯=Nms（match_service 判定时刻 now−judgeAt 的实际值）——
  直接按 N=0 / N>0 分桶，各桶命中率 = hit=1 数 ÷ 判定数。两轮采集的唯一区别就是
  客户端 Sightline.RttReport 开关 → 服务器回溯量，其余（滑块档位/巡逻靶/射手）全同。
  [射击拒绝]（弹匣空/射速/换弹）不计入命中率分母，单独列出供核对采集量。

采集协议（详见 bot_patrol.py 头注）：
  轮 A（回溯关）：PIE 控制台 Sightline.RttReport 0 → 瞄准巡逻路径中点按住开火 30 发
  轮 B（回溯开）：Sightline.RttReport 1 → 同靶同点再 30 发
  两轮日志分开保存，各自跑本脚本，对比两桶命中率。
"""
import argparse
import re
import sys

RE_JUD = re.compile(r"\[射击判定\] shooter=(\d+).*? hit=(\d) .*? 回溯=(\d+)ms")
RE_REJ = re.compile(r"\[射击拒绝\] shooter=(\d+) 原因=(\S+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True, help="服务器日志（含 [射击判定] 整链行）")
    ap.add_argument("--shooter", type=int, help="只统计该射手（缺省=全射手混计）")
    a = ap.parse_args()

    # shooter → {bucket: [judged, hits]}；bucket: "off"(回溯=0) / "on"(回溯>0)
    stats, rejects = {}, {}
    with open(a.server, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if m := RE_JUD.search(ln):
                sid, hit, rewind = int(m.group(1)), int(m.group(2)), int(m.group(3))
                if a.shooter and sid != a.shooter:
                    continue
                s = stats.setdefault(sid, {"off": [0, 0], "on": [0, 0]})
                b = s["off" if rewind == 0 else "on"]
                b[0] += 1
                b[1] += hit
            elif m := RE_REJ.search(ln):
                sid, why = int(m.group(1)), m.group(2)
                if a.shooter and sid != a.shooter:
                    continue
                rejects.setdefault(sid, {})
                rejects[sid][why] = rejects[sid].get(why, 0) + 1

    if not stats:
        print("无 [射击判定] 样本——确认服务器 INFO 档运行且已发生开火")
        sys.exit(1)

    print("══ ep06 命中率分桶（回溯关=回溯0ms / 回溯开=回溯>0ms）══")
    for sid in sorted(stats):
        s = stats[sid]
        print(f"射手 {sid}:")
        for label, key in (("回溯关", "off"), ("回溯开", "on")):
            judged, hits = s[key]
            if judged == 0:
                print(f"  {label}: 无样本")
                continue
            rate = hits / judged * 100
            print(f"  {label}: 判定 {judged} 发 / 命中 {hits} → 命中率 {rate:.1f}%")
        if sid in rejects:
            detail = " ".join(f"{k}×{v}" for k, v in sorted(rejects[sid].items()))
            print(f"  拒绝（不入分母）: {detail}")
    print("\nKPI 口径：两桶唯一差异=客户端 Sightline.RttReport 开关（→服务器回溯量）；滑块/靶/射手全同。")


if __name__ == "__main__":
    main()
