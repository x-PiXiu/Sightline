# -*- coding: utf-8 -*-
"""ep04 · t1 旅程图灯位焦点帧产线 v1（2026-10-23）
背景：S4'-A 有 9 个讲解拍位，t1 原为整图+punch-in——按用户令给每一步出焦点帧
     （t3f 同款工艺：current=粗边保语义色/future=压暗，讲到哪切哪张）。
帧 → 台词对位：
  t1f_0 全图（S4'-01）/ f1 按键+当场动（S4'-02）/ f2 落底账（S4'-03）/ f3 19 号上行（S4'-04）
  f4 服务器权威+17 号+快照回来（S4'-05）/ f5 翻底账（S4'-06）/ f6 ε内+绿回环（S4'-07）
  f7 超 ε（S4'-08）/ f8 断档（S4'-09）/ f9 横幅收口
C6 三分支特写插镜位不变（图管结构，卡管案例）。
数字核对：ε=5cm / 底账 1s×30·硬顶 64（SightlineMatchSession.cpp）· 界 1 米/条
         （match_service kMaxSpeedUuPerSec×kMaxInputGapSec）· 19 号 25B/30Hz（MSG_IDS v4.1）。
用法：python make_ep04_t1_focus.py（帧表可改重渲；PNG 本地不入库）
"""
import re
import subprocess
import time
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent
OUT = ROOT / "trilogy"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"
SCALE = "2"
SK = "sketch=1;curveFitting=1;jiggle=2;"


def box(i, v, x, y, w, h, fill="#ffffff", stroke="#6c8ebf", fs=22, extra=""):
    return (f'<mxCell id="{i}" value="{v}" style="rounded=1;whiteSpace=wrap;fillColor={fill};'
            f'strokeColor={stroke};fontSize={fs};{SK}{extra}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')


def text(i, v, x, y, w, h, fs=18, color="#333333", align="left", bold=False):
    b = "fontStyle=1;" if bold else ""
    return (f'<mxCell id="{i}" value="{v}" style="text;fontSize={fs};{b}fontColor={color};'
            f'align={align};whiteSpace=wrap;" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')


def arrow(i, x1, y1, x2, y2, stroke="#666666", width=3):
    return (f'<mxCell id="{i}" value="" style="edgeStyle=none;strokeColor={stroke};strokeWidth={width};'
            f'endArrow=block;endFill=1;" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/></mxGeometry></mxCell>')


def arrow_o(i, x1, y1, x2, y2, pts=(), stroke="#666666", width=3, dashed=False, label=""):
    d = "dashed=1;dashPattern=8 4;" if dashed else ""
    p = "".join(f'<mxPoint x="{a}" y="{b}"/>' for a, b in pts)
    lb = f'labelBackgroundColor=#ffffff;fontSize=19;' if label else ""
    return (f'<mxCell id="{i}" value="{label}" style="edgeStyle=orthogonalEdgeStyle;strokeColor={stroke};'
            f'strokeWidth={width};endArrow=block;endFill=1;{d}{lb}" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/><Array as="points">{p}</Array></mxGeometry></mxCell>')


BG = ('<mxCell id="bg" value="" style="rounded=0;fillColor=#ffffff;strokeColor=none;" '
      'vertex="1" parent="1"><mxGeometry x="0" y="0" width="1920" height="1080" as="geometry"/></mxCell>')

# 常驻框（不压暗）：标题/图章/泳道容器/脚注
frame_cells = [
    text("title", "一条输入的两段旅程——去程当场动，回程批改重抄", 70, 40, 1400, 60, fs=38, bold=True),
    text("stamp", "t1 灯位焦点帧 · 草案 v1 · 2026-10-23", 1500, 52, 350, 34, fs=18, color="#999999", align="right"),
    (f'<mxCell id="laneC" value="客户端 · 手感侧" style="rounded=0;fillColor=none;strokeColor=#d6b656;'
     f'fontSize=18;fontStyle=1;align=left;verticalAlign=top;spacing=4;" vertex="1" parent="1">'
     f'<mxGeometry x="70" y="120" width="1490" height="620" as="geometry"/></mxCell>'),
    text("foot", "图=代码：ε=5cm / 底账 30·硬顶64 = SightlineMatchSession.cpp · 界 1 米/条 = match_service.cpp · 19/17 号 = MSG_IDS v4.1",
         70, 1020, 1400, 30, fs=15, color="#999999"),
]

GROUPS = {
    # 去程
    "go1": [
        box("f1", "按键 W&#10;（输入进来）", 110, 190, 240, 120, fill="#fff2cc", stroke="#d6b656"),
        arrow("e12", 350, 250, 430, 250),
        box("f2", "当场移动&#10;（预测先行 · 零等待）", 430, 190, 320, 120,
            fill="#fff2cc", stroke="#d6b656", extra="fontStyle=1;"),
    ],
    "go2": [
        arrow("e23", 750, 250, 830, 250),
        box("f3", "落底账&#10;(seq, 位置, 朝向)&#10;1s × 30 条 · 硬顶 64", 830, 190, 300, 140,
            fill="#fff2cc", stroke="#d6b656", fs=20),
    ],
    "go3": [
        arrow("e34", 1130, 250, 1210, 250),
        box("f4", "19 号上行&#10;30Hz · 25B · 只报意图", 1210, 190, 320, 120,
            fill="#fff2cc", stroke="#d6b656"),
        arrow("eUp", 1530, 250, 1590, 260),
    ],
    # 回程：服务器权威 + 快照回来（S4'-05 一拍）
    "srvback": [
        box("srv", "服务器权威（唯一出场）&#10;权威位 = 上次权威位 + 钳后增量&#10;界 = 1 米/条 · 瞬移钳回",
            1590, 180, 260, 160, fill="#e1f5fe", stroke="#0288d1", fs=20),
        arrow_o("eDown", 1720, 340, 1530, 505, pts=[(1720, 505)]),
        box("f5", "快照回来 · 17 号 v2&#10;权威位 + ackedSeq（33ms 拍）", 1210, 440, 320, 130,
            fill="#e1f5fe", stroke="#0288d1"),
    ],
    "ledger": [
        arrow("e56", 1210, 505, 1130, 505),
        box("f6", "拿确认号翻底账&#10;权威位 vs 同号预测位&#10;三方对账", 810, 440, 320, 130,
            fill="#fff2cc", stroke="#d6b656", extra="fontStyle=1;"),
    ],
    # 三分支：ε 内（含绿回环）/ 超 ε / 断档
    "g1": [
        arrow_o("eGate1", 810, 479, 530, 505, pts=[(720, 479), (720, 505)],
                stroke="#82b366", label="&lt; ε"),
        box("b1", "ε 内（5cm）&#10;无感通过——&#10;不碰您的人物", 250, 430, 280, 150,
            fill="#d5e8d4", stroke="#82b366", fs=20),
        arrow_o("retEdge", 250, 505, 590, 360, pts=[(170, 505), (170, 360)],
                stroke="#82b366", width=4),
        text("retLab", "回到预测&#10;手感零等待", 30, 430, 135, 56, fs=20, bold=True, align="right"),
    ],
    "g2": [
        arrow_o("eGate2", 890, 570, 390, 620, pts=[(890, 595), (390, 595)],
                stroke="#b85450", label="≥ ε"),
        box("b2", "超 ε：回滚 + 重放&#10;摆回权威位 →&#10;重放其后输入&#10;→ 回到预测", 250, 620, 280, 150,
            fill="#f8cecc", stroke="#b85450", fs=20, extra="fontColor=#990000;"),
    ],
    "g3": [
        arrow_o("eGate3", 970, 570, 850, 695, pts=[(970, 620)],
                stroke="#b85450", dashed=True, label="断档"),
        box("b3", "断档：全量兜底&#10;回滚到最新权威位&#10;重放 0 帧 · 回到预测", 570, 620, 280, 150,
            fill="#f8cecc", stroke="#b85450", fs=20,
            extra="dashed=1;dashPattern=8 4;fontColor=#990000;"),
    ],
    "ban": [
        (f'<mxCell id="ban" value="抄作业，批改后重抄——卷面还是您的笔迹" '
         f'style="rounded=0;whiteSpace=wrap;fillColor=#f7f4ee;strokeColor=#c8161d;strokeWidth=2;'
         f'fontSize=30;fontStyle=1;align=center;spacing=12;{SK}" vertex="1" parent="1">'
         f'<mxGeometry x="70" y="930" width="1780" height="80" as="geometry"/></mxCell>'),
    ],
}

ORDER = ["go1", "go2", "go3", "srvback", "ledger", "g1", "g2", "g3", "ban"]


def build(cur_groups):
    """current=粗边保语义色 / future=压暗 / 空表=全亮入场帧（C 卡工艺同款）"""
    cells = list(frame_cells)
    future = ORDER[max(ORDER.index(g) for g in cur_groups) + 1:] if cur_groups else []
    for g in ORDER:
        dim = g in future
        cur = g in cur_groups
        for cell in GROUPS[g]:
            if cur:
                # 箭头/横幅：样式里已有 strokeWidth → 原位替换；语义框：默认无该 token → 插进 style 属性内
                if "endArrow=" in cell:
                    cell = re.sub(r"strokeWidth=[\d.]+", "strokeWidth=6", cell)
                elif "fillColor=#f7f4ee" in cell:
                    cell = re.sub(r"strokeWidth=[\d.]+", "strokeWidth=5", cell)
                else:
                    cell = re.sub(r'(style="[^"]*)"', r"\1;strokeWidth=5\"", cell, count=1)
            if dim:
                cell = cell.replace('style="', 'style="opacity=35;textOpacity=35;', 1)
            cells.append(cell)
    body = "\n        ".join([BG] + cells)
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="t1f" id="t1f">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        ' + body +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')


FRAMES = [
    ("t1f_0", []),                       # 全图（S4'-01）
    ("t1f_1", ["go1"]),                  # 按键+当场动（S4'-02）
    ("t1f_2", ["go2"]),                  # 落底账（S4'-03）
    ("t1f_3", ["go3"]),                  # 19 号上行（S4'-04）
    ("t1f_4", ["srvback"]),              # 服务器权威+17 号+快照回来（S4'-05）
    ("t1f_5", ["ledger"]),               # 翻底账（S4'-06）
    ("t1f_6", ["g1"]),                   # ε 内+绿回环（S4'-07）
    ("t1f_7", ["g2"]),                   # 超 ε（S4'-08）
    ("t1f_8", ["g3"]),                   # 断档（S4'-09）
    ("t1f_9", ["ban"]),                  # 横幅收口
]


def render(xml, name):
    tmp = ROOT / f"_tmp_{name}.drawio"
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
    for name, cur in FRAMES:
        render(build(cur), name)
    print("done:", len(list(OUT.glob("t1f_*.png"))), "frames")
