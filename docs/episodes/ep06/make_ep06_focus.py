# -*- coding: utf-8 -*-
"""ep06 · 灯位焦点帧产线 v1（2026-11-01）
工艺 = ep05 make_ep05_focus.py 同款（直接解析已审定 drawio——图=代码零转写误差；
current=粗边强调 / future=压暗 35 / past=常色）。
帧 → 台词对位（第06期-周报脚本.md §2 S3 九拍）：
  t5r_0 全图(S3-00) / t5r_1 历史环 push(S3-01) / t5r_2 心跳+22 号上报(S3-02)
  t5r_3 两物并列旁注(S3-03) / t5r_4 所见 P(now−D)(S3-04) / t5r_5 20 号开火(S3-05)
  t5r_6 judgeAt 恒等式(S3-06) / t5r_7 插值求交+钳制(S3-07/08) / t5r_8 21 号+绿球(S3-09)
数字核对：22 号 6B / 历史环 32 槽≈1.07s / judgeAt=now−min(rtt,1000) / 钳制 kMaxRewindMs
         / 恒等式 judgeAt≡T_key−D / 21 号 22B 含 victimHp（gen.cpp 逐项核）。
用法：python make_ep06_focus.py（PNG 本地不入库）
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

# 常驻单元（不压暗不强调）：背景/标题/图章/图例/泳道头/生命线
T5R_FRAME = {"bg", "title", "stamp", "lg1", "lg1t", "lg2", "lg2t", "lg3", "lg3t", "lg4", "lg4t",
             "cliHead", "srvHead", "cliLife", "srvLife"}
T5R_ORDER = {
    "hist":  ["step4", "e4"],
    "rtt":   ["step1", "e1", "e1b", "step2", "e2", "step3"],
    "meta":  ["note", "note2"],
    "seen":  ["step5"],
    "fire":  ["step6", "e6"],
    "judge": ["step7"],
    "pipe":  ["step8", "e8", "clamp"],
    "feed":  ["step9"],
}
T5R_FRAMES = [
    ("t5r_0", []), ("t5r_1", ["hist"]), ("t5r_2", ["rtt"]), ("t5r_3", ["meta"]),
    ("t5r_4", ["seen"]), ("t5r_5", ["fire"]), ("t5r_6", ["judge"]), ("t5r_7", ["pipe"]),
    ("t5r_8", ["feed"]),
]


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


def build(cells, frame_ids, order, cur_groups, stamp_new):
    """current=粗边强调 / future=压暗 35 / past=常色；frame_ids=常驻。"""
    cur_set = set().union(*[set(order[g]) for g in cur_groups]) if cur_groups else set()
    order_keys = list(order.keys())
    last_idx = max((order_keys.index(g) for g in cur_groups), default=-1)
    future_set = (set().union(*[set(order[g]) for g in order_keys[last_idx + 1:]])
                  if last_idx + 1 < len(order_keys) else set())

    out = []
    for cid, cell in cells.items():
        if cid in frame_ids:
            if cid == "stamp":
                cell = re.sub(r'(value=")[^"]*(")', rf"\g<1>{stamp_new}\g<2>", cell, count=1)
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
    cells = parse_cells(ROOT / "rewind_sequence.drawio")
    stamp_base = "回溯时序图 · 入镜主图 · 焦点帧 {n}/8 · 2026-11-01"
    for i, (name, groups) in enumerate(T5R_FRAMES):
        stamp = stamp_base.format(n=i)
        xml = wrap(build(cells, T5R_FRAME, T5R_ORDER, groups, stamp), name)
        render(xml, name)
        print(f"{name} ✓ ({i}/8)")


if __name__ == "__main__":
    main()
