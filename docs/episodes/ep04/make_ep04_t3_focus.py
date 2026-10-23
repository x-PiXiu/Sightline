# -*- coding: utf-8 -*-
"""ep04 · t3 和解时序（上镜版）灯位焦点帧产线 v1（2026-10-23）
背景：S3'-01 曾写"某框/某箭头亮"——静态 PNG 在剪辑里点不亮局部（v9 审令同款问题），
     正解=C1~C6 同款灯位焦点帧工艺：讲到哪切哪张（current=粗边保语义色/future=压暗）。
内容：对标 reconcile_state.png 的字级密度（每框 3 行）；双端时序布局（16:9 上镜版）。
帧：t3f_0 全图入场 / f1 cli1+msg1 / f2 srv / f3 cheat / f4 msg2 / f5 ban 收口。
数字核对：19 号 25B/30Hz、界 1 米=100uu（match_service kMaxSpeedUuPerSec×kMaxInputGapSec）、
         ackedSeq 只进不退（R11）、17 v2 33ms 含己（契约 v4.1）——渲染前已核。
用法：python make_ep04_t3_focus.py（需 draw.io CLI；重渲单帧改帧表即可）
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


def box(i, v, x, y, w, h, fill="#ffffff", stroke="#6c8ebf", fs=19, extra=""):
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


def loop(i, x1, y1, x2, y2, pts, stroke="#82b366", width=3):
    p = "".join(f'<mxPoint x="{a}" y="{b}"/>' for a, b in pts)
    return (f'<mxCell id="{i}" value="" style="edgeStyle=orthogonalEdgeStyle;strokeColor={stroke};'
            f'strokeWidth={width};endArrow=block;endFill=1;" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/><Array as="points">{p}</Array></mxGeometry></mxCell>')


def chip(i, x, y, fill, stroke, label, dashed=False):
    d = "dashed=1;dashPattern=8 4;" if dashed else ""
    return (box(i, "", x, y, 26, 19, fill=fill, stroke=stroke, extra=d) +
            text(i + "t", label, x + 36, y - 5, 200, 28, fs=16))


# ══ 布局（1920×1080）—— 每框 3 行，对标 reconcile_state 字级密度 ══
BG = ('<mxCell id="bg" value="" style="rounded=0;fillColor=#ffffff;strokeColor=none;" '
      'vertex="1" parent="1"><mxGeometry x="0" y="0" width="1920" height="1080" as="geometry"/></mxCell>')

FRAME_IDS = ["bg", "title", "stamp", "cliHead", "srvHead", "cliLife", "srvLife",
             "lg1", "lg1t", "lg2", "lg2t", "lg3", "lg3t", "fn"]

frame_cells = [
    text("title", "第 4 期 · 和解时序（上镜版）—— 手感先行，权威兜底", 70, 44, 1400, 60, fs=36, bold=True),
    text("stamp", "t3 灯位焦点帧 · 草案 v1 · 2026-10-23", 1490, 56, 360, 34, fs=17, color="#999999", align="right"),
    box("cliHead", "UE 客户端 · 手感侧", 340, 120, 420, 60, fill="#fff2cc", stroke="#d6b656", fs=22),
    box("srvHead", "服务器 · 判定侧", 1160, 120, 420, 60, fill="#e1f5fe", stroke="#0288d1", fs=22),
    arrow("cliLife", 550, 180, 550, 860, stroke="#d6b656", width=2, dashed=True),
    arrow("srvLife", 1370, 180, 1370, 860, stroke="#0288d1", width=2, dashed=True),
    chip("lg1", 1370, 930, "#fff2cc", "#d6b656", "客户端手感侧"),
    chip("lg2", 1370, 972, "#e1f5fe", "#0288d1", "服务器判定侧"),
    chip("lg3", 1370, 1014, "#f8cecc", "#b85450", "非法声明路径", dashed=True),
    text("fn", "全图版 = reconcile_sequence.drawio（定稿 v1）· 对账详版 = reconcile_state.png · 图=代码见 trilogy/README",
         170, 1015, 1150, 30, fs=15, color="#999999"),
]

GROUPS = {
    "cli1": [
        box("cli1", "本地预测执行&#10;按键当场动 · 手感零等待&#10;输入入环形缓冲 (seq, 位置, 朝向) × 1s",
            170, 230, 540, 120, fill="#fff2cc", stroke="#d6b656", fs=19),
    ],
    "msg1": [
        arrow("msg1", 550, 400, 1370, 400),
        text("msg1lab", "19 号 PLAYER_INPUT{seq} · 25B · 30Hz&#10;只报意图，不报位置",
             680, 348, 420, 46, fs=19, align="center"),
    ],
    "srv": [
        box("srv", "权威推算（裁决方 = 服务器）&#10;权威位 = 上次权威位 + 钳后增量&#10;界 = 1 米/条 · ackedSeq 只进不退",
            1080, 430, 540, 140, fill="#e1f5fe", stroke="#0288d1", fs=19),
    ],
    "cheat": [
        arrow("cheatStub", 1370, 570, 1370, 590, stroke="#b85450", width=2, dashed=True),
        box("cheat", "瞬移声明：钳回界上&#10;保方向弃模长——传送打不动权威位&#10;（这套校验 = 第 8 期防作弊的砖）",
            1080, 590, 540, 110, fill="#f8cecc", stroke="#b85450", fs=18,
            extra="dashed=1;dashPattern=8 4;fontColor=#990000;"),
    ],
    "msg2": [
        arrow("msg2", 1370, 750, 550, 750),
        text("msg2lab", "17 号 v2 · 权威位 + ackedSeq · 33ms 拍&#10;含接收者自己（契约 v4.1）",
             640, 700, 480, 46, fs=19, align="center"),
    ],
    "cli2": [
        box("cli2", "拿 ackedSeq 翻底账（本地输入史）&#10;三方对账：权威位 vs 同 seq 预测位&#10;三分支：ε 内无感 / 超 ε 重放 / 断档兜底（详版见 t1）",
            170, 780, 540, 130, fill="#fff2cc", stroke="#d6b656", fs=18),
    ],
    "ret": [
        loop("retEdge", 170, 845, 170, 290, [(140, 845), (140, 290)]),
        text("retLab", "回到预测&#10;手感零等待", 22, 520, 108, 56, fs=17, align="center"),
    ],
    "ban": [
        (f'<mxCell id="ban" value="客户端抄作业，服务器批改——手感先行，权威兜底" '
         f'style="rounded=0;whiteSpace=wrap;fillColor=#f7f4ee;strokeColor=#c8161d;strokeWidth=2;'
         f'fontSize=26;fontStyle=1;align=center;spacing=12;{SK}" vertex="1" parent="1">'
         f'<mxGeometry x="170" y="930" width="1150" height="72" as="geometry"/></mxCell>'),
    ],
}

ORDER = ["cli1", "msg1", "srv", "cheat", "msg2", "cli2", "ret", "ban"]


def build(cur_groups):
    """current=粗边保语义色 / future=压暗 / 空表=全亮入场帧（C 卡工艺同款）"""
    cells = list(frame_cells)
    future = ORDER[max(ORDER.index(g) for g in cur_groups) + 1:] if cur_groups else []

    def cur_style(st):
        if "endArrow=" in st:
            return re.sub(r"strokeWidth=[\d.]+", "strokeWidth=6", st)
        if "rounded=1" in st:
            return re.sub(r"strokeWidth=[\d.]+", "strokeWidth=5", st)
        return st

    for g in ORDER:
        dim = g in future
        cur = g in cur_groups
        for cell in GROUPS[g]:
            if cur:
                cell = re.sub(r"strokeWidth=[\d.]+", "strokeWidth=5", cell) \
                    if "strokeWidth=" in cell and "endArrow=" not in cell else cell
                if "endArrow=" in cell:
                    cell = re.sub(r"strokeWidth=[\d.]+", "strokeWidth=6", cell)
            if dim:
                cell = cell.replace('style="', 'style="opacity=35;textOpacity=35;', 1)
            cells.append(cell)

    body = "\n        ".join([BG] + cells)
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="t3f" id="t3f">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        ' + body +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')


FRAMES = [
    ("t3f_0", []),            # 全图入场
    ("t3f_1", ["cli1", "msg1"]),  # 您按下输入 → 上行
    ("t3f_2", ["srv"]),       # 权威推算
    ("t3f_3", ["cheat"]),     # 瞬移钳回（第 8 期砖）
    ("t3f_4", ["msg2"]),      # 17 号回执
    ("t3f_5", ["ban"]),       # 收口（横幅粗边）
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
    print("done:", len(list(OUT.glob("t3f_*.png"))), "frames")
