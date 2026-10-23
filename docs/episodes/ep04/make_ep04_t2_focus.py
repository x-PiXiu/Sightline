# -*- coding: utf-8 -*-
"""ep04 · t2 插值管线灯位焦点帧产线 v1（2026-10-23）
背景：用户抓 t2 三处缺失（④无输入箭头断链/无输出终点/横幅只讲位置线）——drawio 修完后，
     按 t1f/t3f 先例给 S4'-C 各拍位出焦点帧（current=粗边保语义色/future=压暗，切帧即亮）。
帧 → 台词对位：
  t2f_0 全图（S4'-15）/ f1 ①快照+②滑窗（S4'-17）/ f2 ③渲染点+插值标注句（S4'-18）
  f3 外推保护（S4'-19）/ f4 ④速度+同源分叉（S4'-20）/ f5~f7 第 1/2/3 层（S4'-21 依次亮）
  f8 输出节点+横幅收口（S4'-22 远端平滑输出）
数字核对：25ms/200ms/α0.1·0.25/断15/封420/20~46ms = SightlineCharacter.cpp 现行代码 + devlog fc82092。
用法：python make_ep04_t2_focus.py（帧表可改重渲；PNG 本地不入库）
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


def arrow(i, x1, y1, x2, y2, stroke="#666666", width=3, dashed=False):
    d = "dashed=1;dashPattern=8 4;" if dashed else ""
    return (f'<mxCell id="{i}" value="" style="edgeStyle=none;strokeColor={stroke};strokeWidth={width};'
            f'endArrow=block;endFill=1;{d}" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/></mxGeometry></mxCell>')


def arrow_o(i, x1, y1, x2, y2, pts=(), stroke="#666666", width=3, label=""):
    p = "".join(f'<mxPoint x="{a}" y="{b}"/>' for a, b in pts)
    lb = "labelBackgroundColor=#ffffff;fontSize=18;" if label else ""
    return (f'<mxCell id="{i}" value="{label}" style="edgeStyle=orthogonalEdgeStyle;strokeColor={stroke};'
            f'strokeWidth={width};endArrow=block;endFill=1;{lb}" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/><Array as="points">{p}</Array></mxGeometry></mxCell>')


BG = ('<mxCell id="bg" value="" style="rounded=0;fillColor=#ffffff;strokeColor=none;" '
      'vertex="1" parent="1"><mxGeometry x="0" y="0" width="1920" height="1080" as="geometry"/></mxCell>')

# 常驻框（不压暗）：标题/图章/⑤容器（含标题）/自机豁免注/豁免虚线/图例/脚注
frame_cells = [
    text("title", "远端插值管线——窗口插值 + 三层滤波", 70, 40, 1400, 60, fs=38, bold=True),
    text("stamp", "t2 灯位焦点帧 · 草案 v1 · 2026-10-23", 1500, 52, 350, 34, fs=18, color="#999999", align="right"),
    (f'<mxCell id="fil" value="⑤ 三层滤波——三层噪声，三层各管一段" '
     f'style="rounded=0;whiteSpace=wrap;fillColor=none;strokeColor=#333333;fontSize=22;fontStyle=1;'
     f'align=right;verticalAlign=top;spacing=6;spacingRight=24;" vertex="1" parent="1">'
     f'<mxGeometry x="80" y="620" width="1750" height="270" as="geometry"/></mxCell>'),
    text("self", "本地自机豁免：自机走对账（旅程图），缓冲恒空（契约 v4.1 含己）",
         700, 440, 660, 40, fs=18, color="#999999"),
    arrow("eSelf", 700, 480, 600, 480, stroke="#999999", width=2, dashed=True),
    box("lg1c", "", 700, 505, 26, 19, fill="#e1f5fe", stroke="#0288d1", fs=12),
    text("lg1t", "服务器快照", 736, 499, 160, 28, fs=15),
    box("lg2c", "", 1000, 505, 26, 19, fill="#fff2cc", stroke="#d6b656", fs=12),
    text("lg2t", "客户端管线", 1036, 499, 160, 28, fs=15),
    box("lg3c", "", 700, 560, 26, 19, fill="#d5e8d4", stroke="#82b366", fs=12),
    text("lg3t", "滤波层", 736, 554, 120, 28, fs=15),
    box("lg4c", "", 1000, 560, 26, 19, fill="#f8cecc", stroke="#b85450", fs=12,
        extra="dashed=1;dashPattern=8 4;"),
    text("lg4t", "外推保护", 1036, 554, 120, 28, fs=15),
    text("foot", "图=代码：kInterpDelaySec=25ms / MaxExtrapolation=200ms / VelAlpha 0.1·0.25 / 断 15 / 封 420 = SightlineCharacter.cpp TickRemoteInterpolation",
         70, 1020, 1500, 30, fs=15, color="#999999"),
]

GROUPS = {
    "p12": [
        box("p1", "① 快照到达&#10;33ms 一拍 · TCP 有序", 80, 150, 330, 120,
            fill="#e1f5fe", stroke="#0288d1"),
        arrow("e12", 410, 210, 510, 210),
        box("p2", "② 滑动窗口&#10;SnapA / SnapB 两点&#10;新到顶掉最老", 510, 150, 330, 140,
            fill="#fff2cc", stroke="#d6b656", fs=22),
    ],
    "p3": [
        arrow("e23", 840, 210, 940, 210),
        box("p3", "③ 渲染点 = now − 25ms&#10;窗口内线性插值&#10;（取值点落在两拍之间）", 940, 150, 380, 140,
            fill="#fff2cc", stroke="#d6b656", extra="fontStyle=1;"),
        text("rail", "窗口内线性插值——取值点始终位于两个快照点之间", 510, 330, 810, 44,
             fs=24, color="#d6b656", bold=True),
    ],
    "extrap": [
        arrow("e34", 1320, 220, 1450, 220, stroke="#b85450", dashed=True),
        box("p4", "快照迟到&#10;按末段速度外推&#10;上限 200ms（防过冲穿墙）", 1450, 140, 380, 160,
            fill="#f8cecc", stroke="#b85450", fs=20,
            extra="dashed=1;dashPattern=8 4;fontColor=#990000;"),
    ],
    "vel": [
        arrow_o("eFork", 510, 262, 450, 420, pts=[(450, 262)], label="同一对快照 · 两条线同源"),
        box("p5", "④ 动画速度 = Δ位移 / Δack 拍&#10;按发端输入时序计量（非到达时刻）&#10;——到达抖动不影响速度值",
            80, 420, 520, 150, fill="#fff2cc", stroke="#d6b656"),
        arrow("e5fil", 340, 570, 340, 680),
    ],
    "fil1": [
        box("fl1", "第 1 层 · 帧到达抖动&#10;由 25ms 渲染延迟余量吸收&#10;（窗口机制，无需滤波器）",
            120, 680, 520, 170, fill="#d5e8d4", stroke="#82b366", fs=20),
    ],
    "fil2": [
        arrow("eFil12", 640, 765, 710, 765, stroke="#82b366"),
        box("fl2", "第 2 层 · 发端上报间隔 20~46ms 抖动&#10;非对称低通：上升 α0.1 滤抖动&#10;下降 α0.25 快速收敛",
            710, 680, 530, 170, fill="#d5e8d4", stroke="#82b366", fs=20),
    ],
    "fil3": [
        arrow("eFil23", 1240, 765, 1310, 765, stroke="#82b366"),
        box("fl3", "第 3 层 · 停步残速&#10;速度 &lt; 15 归零&#10;上限钳制 420（防误触满速动画）",
            1310, 680, 490, 170, fill="#d5e8d4", stroke="#82b366", fs=20),
    ],
    "ban": [
        box("outNode", "远端角色 · 平滑输出&#10;位置 = ③ 插值点 · 速度 = ⑤ 滤波后值",
            1400, 505, 430, 110, fill="#fff2cc", stroke="#d6b656", fs=18),
        arrow_o("out1", 1320, 280, 1400, 560, pts=[(1370, 280), (1370, 560)]),
        arrow_o("out2", 1800, 765, 1830, 560, pts=[(1860, 765), (1860, 560)]),
        (f'<mxCell id="ban" value="位置走窗口插值，速度走滤波估计——远端平滑驱动" '
         f'style="rounded=0;whiteSpace=wrap;fillColor=#f7f4ee;strokeColor=#c8161d;strokeWidth=2;'
         f'fontSize=30;fontStyle=1;align=center;spacing=12;{SK}" vertex="1" parent="1">'
         f'<mxGeometry x="70" y="930" width="1780" height="80" as="geometry"/></mxCell>'),
    ],
}

ORDER = ["p12", "p3", "extrap", "vel", "fil1", "fil2", "fil3", "ban"]


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
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="t2f" id="t2f">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        ' + body +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')


FRAMES = [
    ("t2f_0", []),                     # 全图（S4'-15）
    ("t2f_1", ["p12"]),                # ①快照+②滑窗（S4'-17）
    ("t2f_2", ["p3"]),                 # ③渲染点+插值标注（S4'-18）
    ("t2f_3", ["extrap"]),             # 外推保护（S4'-19）
    ("t2f_4", ["vel"]),                # ④速度+同源分叉（S4'-20）
    ("t2f_5", ["fil1"]),               # 第 1 层（S4'-21）
    ("t2f_6", ["fil2"]),               # 第 2 层
    ("t2f_7", ["fil3"]),               # 第 3 层
    ("t2f_8", ["ban"]),                # 输出节点+横幅收口（S4'-22）
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
    print("done:", len(list(OUT.glob("t2f_*.png"))), "frames")
