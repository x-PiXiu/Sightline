# -*- coding: utf-8 -*-
"""ep05 · 灯位焦点帧产线 v1（2026-10-29）
工艺 = ep04 make_ep04_t1_focus.py 同款（current=粗边强调 / future=压暗 35 / past=常色），
差异 = 本产线**直接解析已审定 drawio**（不重抄单元格）——图=代码零转写误差。
帧 → 台词对位（第05期-周报脚本.md §2）：
  t4f_0 全图(S3-00) / f1 IA_Fire(S3-01) / f2 20 号空包(S3-02) / f3 四道门(S3-03；S3-04=日志插入无帧)
  f4 hitscan(S3-05) / f5 结算(S3-06) / f6 21 号广播(S3-08 前半) / f7 横幅收口(S3-08 尾)
  fcf_0 全图(S4-01) / f1 分支①射手(S4-02) / f2 分支②受击(S4-03) / f3 分支③他人(S4-04)
  f4 分支④弹孔(S4-05) / f5 横幅收口(S4-06)
数字核对：空 payload/22B=MSG_IDS v4.2 · 四道门 95ms=fire_judge.h · 命中体 r=34=烘焙合同
         · 弹孔池 64=DecalManager · 眼位 8/8/62=镜像合同（hitscan_math 已同步）。
用法：python make_ep05_focus.py（PNG 本地不入库）
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

# 常驻单元（不压暗不强调）：背景/标题/图章/图例/泳道头/生命线/脚注
T4F_FRAME = {"bg", "title", "stamp", "lg1", "lg1t", "lg2", "lg2t", "lg3", "lg3t",
             "cliHead", "srvHead", "cliLife", "srvLife", "foot"}
T4F_ORDER = {
    "fire":      ["cliBox1"],
    "up20":      ["msg1", "msg1lab", "msg1note"],
    "gates":     ["srvGates", "rejConn", "rejBox", "rejNote"],
    "hitscan":   ["srvHit"],
    "settle":    ["srvSettle"],
    "broadcast": ["msg2", "msg2lab", "msg2note", "fanE1", "fanE2", "fanE3", "fan1", "fan2", "fan3"],
    "banner":    ["concl"],
}
T4F_FRAMES = [
    ("t4f_0", []), ("t4f_1", ["fire"]), ("t4f_2", ["up20"]), ("t4f_3", ["gates"]),
    ("t4f_4", ["hitscan"]), ("t4f_5", ["settle"]), ("t4f_6", ["broadcast"]), ("t4f_7", ["banner"]),
]

FCF_FRAME = {"bg", "title", "stamp", "lg1", "lg1t", "lg2", "lg2t", "lg3", "lg3t",
             "lg4", "lg4t", "colL", "colR", "note", "foot"}
FCF_ORDER = {
    "uplink":  ["c1", "d1", "c2", "c3", "c4", "e1lab", "e2lab", "e3lab", "e1", "e2", "e3", "e4"],
    "branch1": ["s1", "fe1", "f1"],
    "branch2": ["fe2", "f2"],
    "branch3": ["fe3", "f3"],
    "branch4": ["fe4", "f4"],
    "midconn": ["midLab", "midConn"],
    "banner":  ["concl"],
}
FCF_FRAMES = [
    ("fcf_0", []), ("fcf_1", ["branch1"]), ("fcf_2", ["branch2"]), ("fcf_3", ["branch3"]),
    ("fcf_4", ["branch4"]), ("fcf_5", ["midconn", "banner"]),
]


def parse_cells(path):
    """drawio → {id: 完整 mxCell 串}（顶点与边一视同仁）。"""
    data = pathlib.Path(path).read_text(encoding="utf-8")
    cells = {}
    for m in re.finditer(r'<mxCell id="([^"]+)".*?</mxCell>', data, re.S):
        cells[m.group(1)] = m.group(0)
    return cells


def build(cells, frame_ids, order, cur_groups, stamp_new):
    """current=粗边强调 / future=压暗 35 / past=常色；frame_ids=常驻。"""
    def all_in(g):
        return set(order[g])
    cur_set = set().union(*[all_in(g) for g in cur_groups]) if cur_groups else set()
    order_keys = list(order.keys())
    last_idx = max((order_keys.index(g) for g in cur_groups), default=-1)
    future_set = (set().union(*[all_in(g) for g in order_keys[last_idx + 1:]])
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


def order_all(order):
    s = set()
    for ids in order.values():
        s.update(ids)
    return s


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
        time.sleep(1.2)
    else:
        raise RuntimeError(f"渲染失败: {name}")
    tmp.unlink()
    print(f"  {png.name}  {png.stat().st_size // 1024}KB")


if __name__ == "__main__":
    t4 = parse_cells(ROOT / "fire_sequence.drawio")
    for name, cur in T4F_FRAMES:
        body = build(t4, T4F_FRAME, T4F_ORDER, cur, "t4 灯位焦点帧 · 草案 v1 · 2026-10-29")
        render(wrap(body, name), name)
    fc = parse_cells(ROOT / "fire_client_flow.drawio")
    for name, cur in FCF_FRAMES:
        body = build(fc, FCF_FRAME, FCF_ORDER, cur, "fcf 灯位焦点帧 · 草案 v1.1 · 2026-10-29")
        render(wrap(body, name), name)
    print("done:", len(list(OUT.glob("*.png"))), "frames")
