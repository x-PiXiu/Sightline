# -*- coding: utf-8 -*-
"""ep07 · 灯位焦点帧产线 v1（2026-11-05）
工艺 = ep06 make_ep06_focus.py 同款（直接解析已审定 drawio——图=代码零转写误差；
current=粗边强调 / future=压暗 35 / past=常色）。
三源三套：
  对局状态机.drawio → t7a_0~5（S2 六拍：全图/存活/死亡/重生选点/无敌开火即破/收束）
  结算流程.drawio   → t7b_0~1（S3 两拍：服务端带/客户端带对账亮）
  GP框架对象关系图.drawio → gp_0~1（S3 两态：全景/映射虚线亮）
帧 → 台词对位（第07期-周报脚本.md §2 S2/S3）。
数字核对：3s 重生 / 2s 无敌·开火即破 / 150cm 选点排斥 / 十杀上限（fire_judge.h 逐项核）。
用法：python make_ep07_focus.py（PNG 本地不入库——媒体纪律）
"""
import re
import subprocess
import time
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / "focus"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"
SCALE = "2"

# ══ 对局状态机（S2 生死簿六拍）══
T7A_FRAME = {"bg", "title", "subtitle", "illegal", "legend", "concl"}
T7A_ORDER = {
    "start":   ["sLobby", "e1"],
    "alive":   ["sAlive"],
    "dead":    ["sDead", "e2"],
    "respawn": ["e3", "sInvuln"],
    "break_":  ["e4"],
    "ending":  ["sEnd", "e5"],
    "closing": ["sClose", "e6", "e7", "e8"],
    "reject":  ["sReject", "e9"],
}
T7A_FRAMES = [
    ("t7a_0", []), ("t7a_1", ["alive"]), ("t7a_2", ["dead"]),
    ("t7a_3", ["respawn"]), ("t7a_4", ["break_"]), ("t7a_5", []),
]

# ══ 结算流程（S3 账目两拍）══
T7B_FRAME = {"bg", "title", "legend", "laneS", "laneC", "concl"}
T7B_ORDER = {
    "srv": ["s1", "s2", "s3", "s4", "es1", "es2", "es3", "esNo"],
    "cli": ["c1", "c2", "c3", "c4", "cBad", "ec1", "ec2", "ec3", "ec4", "ecBad"],
}
T7B_FRAMES = [("t7b_0", ["srv"]), ("t7b_1", ["cli"])]

# ══ GP 框架对照（S3 两态）══
GP_FRAME = {"bg", "title", "legend", "bandUE", "bandS",
            "ueGM", "ueGS", "uePS", "uePC", "uePawn",
            "slMatch", "slSync", "slRow", "slInput", "slPawn",
            "eGSGM", "ePCPS", "ePCPawn", "concl"}
GP_ORDER = {"map": ["mGM", "mGS", "mPS", "mPC", "mPawn"]}
GP_FRAMES = [("gp_0", []), ("gp_1", ["map"])]


def parse_cells(path):
    """drawio → {id: 完整 mxCell 串}（顶点与边一视同仁）。"""
    data = pathlib.Path(path).read_text(encoding="utf-8")
    cells = {}
    for m in re.finditer(r'<mxCell id="([^"]+)".*?</mxCell>', data, re.S):
        cells[m.group(1)] = m.group(0)
    return cells


def order_all(order):
    s = set()
    for ids in order.values():
        s.update(ids)
    return s


def build(cells, frame_ids, order, cur_groups):
    """current=粗边强调 / future=压暗 35 / past=常色；frame_ids=常驻。"""
    cur_set = set().union(*[set(order[g]) for g in cur_groups]) if cur_groups else set()
    order_keys = list(order.keys())
    last_idx = max((order_keys.index(g) for g in cur_groups), default=-1)
    future_set = (set().union(*[set(order[g]) for g in order_keys[last_idx + 1:]])
                  if cur_groups and last_idx + 1 < len(order_keys) else set())

    out = []
    for cid, cell in cells.items():
        if cid in frame_ids:
            out.append(cell)
            continue
        if cid not in order_all(order):
            continue
        if cid in cur_set:
            if "endArrow=" in cell or "strokeWidth=" in cell:
                cell = re.sub(r"strokeWidth=[\d.]+", "strokeWidth=5", cell, count=1)
            else:
                cell = re.sub(r'(style="[^"]*)"', r'\1;strokeWidth=5"', cell, count=1)
        elif cid in future_set:
            cell = cell.replace('style="', 'style="opacity=35;textOpacity=35;', 1)
        out.append(cell)
    return out


def wrap(body, name):
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="' + name + '" id="' + name + '">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        '
            + "\n        ".join(body) + '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')


def render(xml, name):
    tmp = OUT / f"_tmp_{name}.drawio"
    png = OUT / f"{name}.png"
    tmp.write_text(xml, encoding="utf-8")
    for _ in range(3):
        before = png.stat().st_mtime if png.exists() else 0
        subprocess.run([DRAWIO, "-x", "-f", "png", "-s", SCALE, "-o", str(png), str(tmp)],
                       capture_output=True)
        if png.exists() and png.stat().st_mtime > before:
            break
        time.sleep(1)
    assert png.exists(), f"导出失败: {name}"
    tmp.unlink()
    return png


def main():
    total = len(T7A_FRAMES) + len(T7B_FRAMES) + len(GP_FRAMES)
    n = 0
    for name, groups in T7A_FRAMES:
        cells = parse_cells(ROOT / "对局状态机.drawio")
        xml = wrap(build(cells, T7A_FRAME, T7A_ORDER, groups), name)
        render(xml, name)
        n += 1
        print(f"{name} ✓ ({n}/{total})")
    for name, groups in T7B_FRAMES:
        cells = parse_cells(ROOT / "结算流程.drawio")
        xml = wrap(build(cells, T7B_FRAME, T7B_ORDER, groups), name)
        render(xml, name)
        n += 1
        print(f"{name} ✓ ({n}/{total})")
    for name, groups in GP_FRAMES:
        cells = parse_cells(ROOT / "GP框架对象关系图.drawio")
        xml = wrap(build(cells, GP_FRAME, GP_ORDER, groups), name)
        render(xml, name)
        n += 1
        print(f"{name} ✓ ({n}/{total})")


if __name__ == "__main__":
    main()
