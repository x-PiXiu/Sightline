# -*- coding: utf-8 -*-
"""第 4 期思路卡产线 v3：6 张 16:9 卡 × 灯位焦点序列（1920×1080，scale2≈3842×2162 渲染）
设计律（用户 2026-10-22 九审令三轮）：
  ①一卡一思路——卡标题=思路句，正文=具名案例作证；
  ②静态卡+口播=枯燥——每卡拆灯位焦点帧，讲到哪亮哪（current=粗边保语义色/future=压暗）；
  ③**证据物纪律（v3 新增）**：每帧配且仅配一个具象证据物——口播指着的名词，画面上有对应的物
    （C1 字节账本条 / C2 向量撞界线 / C3 载荷字段条 / C5 字节流切帧+队列积木 / C6 底账条）；
    不加文字阅读量；C4 清单卡简洁保持，节奏靠剪辑逐格切。
R1：25B/4B seq/100uu/ε=5cm/1s×30 为真；字段名级排布与 C6 坐标标"示意"。
"""
import re, subprocess, time, pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep04")
OUT = ROOT / "cards"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"
SCALE = "2"

SK = "sketch=1;curveFitting=1;jiggle=2;"

def box(i, v, x, y, w, h, fill="#ffffff", stroke="#6c8ebf", fs=26, extra=""):
    return (f'<mxCell id="{i}" value="{v}" style="rounded=1;whiteSpace=wrap;fillColor={fill};'
            f'strokeColor={stroke};fontSize={fs};{SK}{extra}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def text(i, v, x, y, w, h, fs=24, color="#333333", align="left", bold=False):
    b = "fontStyle=1;" if bold else ""
    return (f'<mxCell id="{i}" value="{v}" style="text;fontSize={fs};{b}fontColor={color};'
            f'align={align};whiteSpace=wrap;" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def arrow(i, x1, y1, x2, y2, stroke="#666666", width=3, pts=None, dashed=False):
    d = "dashed=1;dashPattern=8 4;" if dashed else ""
    p = f'<Array as="points">{"".join(f"<mxPoint x={a} y={b}/>" for a, b in pts)}</Array>' if pts else ""
    return (f'<mxCell id="{i}" value="" style="edgeStyle=none;strokeColor={stroke};strokeWidth={width};'
            f'endArrow=block;endFill=1;{d}" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/>{p}</mxGeometry></mxCell>')

def banner(i, v, x, y, w, h, fs=32):
    return (f'<mxCell id="{i}" value="{v}" style="rounded=0;whiteSpace=wrap;fillColor=#f7f4ee;'
            f'strokeColor=#c8161d;strokeWidth=2;fontSize={fs};fontStyle=1;align=center;spacing=12;{SK}"'
            f' vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def page(cells, title, stamp):
    # 坑：drawio 导出裁到内容边界 → 满页白底垫层强制 16:9。
    bg = ('<mxCell id="bg" value="" style="rounded=0;fillColor=#ffffff;strokeColor=none;" '
          'vertex="1" parent="1"><mxGeometry x="0" y="0" width="1920" height="1080" as="geometry"/></mxCell>')
    body = "\n        ".join([bg, text("title", title, 70, 52, 1400, 60, 44, bold=True),
                             text("stamp", stamp, 1500, 60, 350, 40, 20, "#999999", "right")] + cells)
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="cards" id="cards">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        ' + body +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')

# ================= C1 意图卡（+字节账本条） =================
c1_cells = [
    text("hl", "以前：报位置", 70, 160, 820, 44, 26, bold=True),
    text("hr", "现在：报意图", 1030, 160, 820, 44, 26, bold=True),
    box("a1", "客户端：我在 (367, 629)", 70, 220, 820, 105, fs=28),
    arrow("arr1", 480, 328, 480, 368),
    box("a2", "服务器：原样入库&#10;记账的——说什么，信什么", 70, 372, 820, 125, "#f2f2f2", "#999999", 26),
    box("b1", "客户端：我按了前进 · seq 1303", 1030, 220, 820, 105, "#fff2cc", "#d6b656", 28),
    arrow("arr2", 1440, 328, 1440, 368),
    box("b2", "服务器：自己推算权威位置&#10;上次权威位 + 钳制后增量", 1030, 372, 820, 125, "#e1f5fe", "#0288d1", 26),
    # 证据物：19 号字节账本条（字段名级，示意）
    text("blabel", "19 PLAYER_INPUT · 25 字节", 70, 528, 420, 32, 22, bold=True),
    box("b0", "msgid 19", 70, 568, 300, 64, "#1f4e79", "#1f4e79", 22, "fontColor=#ffffff;"),
    box("bseq", "seq · 4B", 378, 568, 300, 64, "#fff2cc", "#d6b656", 22),
    box("bkey", "按键", 686, 568, 360, 64, fs=22),
    box("bmv", "移动 X / Y", 1054, 568, 360, 64, fs=22),
    box("byaw", "朝向 yaw / pitch", 1422, 568, 428, 64, fs=22),
    banner("bn", "位置是结论，意图是事实——结论能造假，事实造不了假", 70, 680, 1780, 100, 34),
    text("ft", "其余 21B 字段名级示意 · seq 4B 为契约实值 · 契约 v4.1", 70, 830, 1780, 36, 22, "#999999"),
]
C1 = (page(c1_cells, "位置会被骗，意图骗不了", "第 4 期 · 思路卡 1 / 6"),
      {"g_old": ["hl", "a1", "arr1", "a2"], "g_new": ["hr", "b1", "arr2", "b2"],
       "g_bytes": ["blabel", "b0", "bseq", "bkey", "bmv", "byaw"], "g_bn": ["bn"]},
      [("c1_f1", ["g_old"]), ("c1_f2", ["g_new"]), ("c1_f2b", ["g_bytes"]), ("c1_f3", ["g_bn"])])

# ================= C2 钳制卡（+向量撞界线） =================
c2_cells = [
    text("jlab", "上限 = 100uu / 条 ≈ 1 米", 560, 128, 240, 30, 20, "#c8161d", "center", bold=True),
    arrow("jline", 680, 162, 680, 568, "#c8161d", 2, dashed=True),
    box("r1s", "声明 80uu", 70, 180, 280, 100, fs=24),
    arrow("r1v", 380, 230, 620, 230, "#82b366", 4),
    text("r1t", "80uu", 440, 196, 120, 26, 20, "#1f6b30", "center"),
    box("r1d", "放行 ✓ —— 上限内，老实走", 900, 180, 950, 100, "#d5e8d4", "#82b366", 26),
    box("r2s", "声明 120uu", 70, 320, 280, 100, fs=24),
    arrow("r2g", 680, 370, 740, 370, "#999999", 2, dashed=True),
    arrow("r2v", 380, 370, 740, 370, "#d6b656", 4),
    text("r2cut", "✂", 655, 340, 40, 30, 24, "#c8161d", "center"),
    text("r2t", "120uu", 440, 336, 140, 26, 20, "#7f6000", "center"),
    box("r2d", "只认 100uu —— 方向保留，超出不认", 900, 320, 950, 100, "#fff2cc", "#d6b656", 26),
    box("r3s", "声明 5000uu", 70, 460, 280, 100, fs=24),
    arrow("r3g", 680, 510, 790, 510, "#999999", 2, dashed=True),
    arrow("r3v", 380, 510, 790, 510, "#b85450", 4),
    text("r3brk", "⁄⁄", 742, 488, 40, 30, 26, "#999999", "center"),
    text("r3t", "5000uu ≫ 画幅", 420, 476, 200, 26, 20, "#990000", "center"),
    box("r3d", "只认 100uu —— 传送声明打不动权威位", 900, 460, 950, 100, "#f8cecc", "#b85450", 26),
    box("why", "上限为什么不按「收到间隔」放宽？&#10;滑块一开，输入挤在一起到达：间隔 1~5ms，每条仍装约 16ms 的位移——&#10;按收到间隔放宽，误伤的全是诚实玩家。&#10;制度只认输入本身，不认收件的时间。",
        70, 620, 1780, 170, "#f7f4ee", "#c8161d", 26),
    text("ft", "上限 = 1000uu/s × 0.1s = 100uu/条 ≈ 1 米（丢帧按 gap 放大，封顶 3 倍）· 向量长度为示意 · 完整校验 = ep08", 70, 830, 1780, 36, 22, "#999999"),
]
C2 = (page(c2_cells, "防线，不建在被污染的观测上", "第 4 期 · 思路卡 2 / 6"),
      {"g_r1": ["r1s", "r1v", "r1t", "r1d"], "g_r2": ["r2s", "r2g", "r2v", "r2cut", "r2t", "r2d"],
       "g_r3": ["r3s", "r3g", "r3v", "r3brk", "r3t", "r3d"], "g_why": ["why"]},
      [("c2_f1", ["g_r1"]), ("c2_f2", ["g_r2"]), ("c2_f3", ["g_r3"]), ("c2_f4", ["g_why"])])

# ================= C3 快照三件套卡（+载荷字段条） =================
c3_cells = [
    box("top", "17 v2 快照 —— 服务器 → 客户端，每 33 毫秒一拍", 70, 165, 1780, 80, "#e1f5fe", "#0288d1", 30),
    box("i1", "① 权威位&#10;&#10;服务器自己算的&#10;不是您报的数", 70, 285, 560, 270, fs=26),
    box("i2", "② ackedSeq 确认号&#10;&#10;认到您第几号输入&#10;对账的钥匙", 680, 285, 560, 270, fs=26),
    box("i3", "③ 含您自己&#10;&#10;您的权威位 + 确认号&#10;原样打包带回", 1290, 285, 560, 270, fs=26),
    box("pay", "17 v2 载荷（字段名级示意）：　[权威位 x / y / z]　　[ackedSeq 确认号]　　[… 含接收者自己]",
        70, 595, 1780, 72, "#f8fbff", "#6c8ebf", 24),
    banner("bn", "三样备齐 = 对账的全部凭据&#10;下半圈，客户端拿它们批改作业", 70, 705, 1780, 120, 32),
]
C3 = (page(c3_cells, "33 毫秒一拍，三样一起发", "第 4 期 · 思路卡 3 / 6"),
      {"g_top": ["top", "pay"], "g_i1": ["i1"], "g_i2": ["i2"], "g_i3": ["i3"], "g_bn": ["bn"]},
      [("c3_f1", ["g_top"]), ("c3_f2", ["g_i1"]), ("c3_f3", ["g_i2"]), ("c3_f4", ["g_i3"]), ("c3_f5", ["g_bn"])])

# ================= C4 main 四样卡（清单卡，简洁保持） =================
c4_cells = [
    box("m1", "接新客&#10;Acceptor 收连接 → 挑 worker&#10;（最少连接计数）", 70, 190, 860, 240, fs=26),
    box("m2", "打拍子&#10;33ms tick · 最小堆定时器&#10;实测拍距 33.0ms", 990, 190, 860, 240, fs=26),
    box("m3", "跑业务&#10;Lobby / Match 单线程串行&#10;一行未改 · 45×3 单测不碰", 70, 470, 860, 240, "#ffe6cc", "#c8161d", 26),
    box("m4", "编码回包&#10;FrameWriter → outbound 缓冲&#10;main 只碰缓冲，不碰 socket", 990, 470, 860, 240, fs=26),
    banner("bn", "收字节的重活，整个交给 worker——IO 搬家，业务不动", 70, 780, 1780, 100, 32),
    text("ft", "kIoThreads = 0 / 2 双拓扑同套代码 · 业务层 git diff 零出现", 70, 920, 1780, 40, 22, "#999999"),
]
C4 = (page(c4_cells, "网络层搬家：main 只留四样", "第 4 期 · 思路卡 4 / 6"),
      {"g_m1": ["m1"], "g_m2": ["m2"], "g_m3": ["m3"], "g_m4": ["m4"], "g_bn": ["bn"]},
      [("c4_full", []), ("c4_f1", ["g_m1"]), ("c4_f2", ["g_m2"]), ("c4_f3", ["g_m3"]), ("c4_f4", ["g_m4"]), ("c4_f5", ["g_bn"])])

# ================= C5 一帧的旅程（+字节流切帧 + 队列积木） =================
c5_cells = [
    box("bandW", "", 60, 170, 830, 740, "#f6fbf4", "#82b366", 14),
    text("bandWl", "IO Worker 线程", 80, 182, 500, 30, 22, "#1f6b30", bold=True),
    box("bandM", "", 1030, 170, 830, 740, "#f8fbff", "#6c8ebf", 14),
    text("bandMl", "main 线程", 1050, 182, 500, 30, 22, "#1f4e79", bold=True),
    box("s1", "① 收字节", 100, 230, 750, 80, fs=26),
    box("s2", "② TryDecode 切帧", 100, 330, 750, 80, fs=26),
    # 证据物：字节流 → 切 → 完整帧（g_s2 组内）
    box("bsq1", "", 150, 432, 26, 26, "#e8e8e8", "#999999", 14), box("bsq2", "", 184, 432, 26, 26, "#e8e8e8", "#999999", 14),
    box("bsq3", "", 218, 432, 26, 26, "#e8e8e8", "#999999", 14), box("bsq4", "", 252, 432, 26, 26, "#e8e8e8", "#999999", 14),
    box("bsq5", "", 286, 432, 26, 26, "#e8e8e8", "#999999", 14), box("bsq6", "", 320, 432, 26, 26, "#e8e8e8", "#999999", 14),
    arrow("bar", 356, 445, 402, 445, "#666666", 2),
    box("bfr", "完整帧", 410, 424, 110, 42, "#fff2cc", "#d6b656", 18),
    text("bsqcap", "字节流 → 切", 150, 464, 220, 24, 16, "#999999"),
    box("s3", "③ 投门 → frame_queue", 100, 490, 750, 100, "#fff2cc", "#d6b656", 26),
    # 证据物：队列积木（g_s3 组内）
    box("q1", "", 620, 525, 50, 30, "#fff2cc", "#d6b656", 14), box("q2", "", 680, 525, 50, 30, "#fff2cc", "#d6b656", 14),
    box("q3", "", 740, 525, 50, 30, "#fff2cc", "#d6b656", 14),
    box("s6", "⑥ EPOLLOUT 发送", 100, 660, 750, 85, fs=26),
    box("s4", "④ 33ms tick 统一消费", 1070, 230, 750, 80, fs=26),
    box("s5", "⑤ FrameWriter 编码 → outbound 缓冲", 1070, 330, 750, 100, fs=26),
    box("s5n", "main 全程不碰 socket", 1070, 460, 750, 80, "#f8fbff", "#999999", 24, "dashed=1;"),
    arrow("a12", 475, 312, 475, 326),
    arrow("a23", 770, 412, 770, 486),
    arrow("a34", 850, 540, 1070, 270, "#d6b656", 5, pts=[(940, 540), (940, 270)]),
    text("lb34", "唯一门&#10;frame_queue", 862, 370, 170, 60, 22, "#7f6000", "center", bold=True),
    arrow("a45", 1445, 312, 1445, 326),
    arrow("a56", 1820, 380, 850, 702, "#6c8ebf", 4, pts=[(1890, 380), (1890, 702)]),
    text("lb56", "outbound 缓冲&#10;+ EPOLLOUT 挂摘", 1500, 720, 300, 60, 22, "#1f4e79", "center", bold=True),
    banner("bn", "跨线程各走一次：进门——帧队列（唯一门）；出手——outbound。锁只守门口这一点", 60, 940, 1800, 90, 28),
]
C5 = (page(c5_cells, "一帧的旅程：门，就这一扇", "第 4 期 · 思路卡 5 / 6"),
      {"g_s1": ["s1"], "g_s2": ["s2", "bsq1", "bsq2", "bsq3", "bsq4", "bsq5", "bsq6", "bar", "bfr", "bsqcap"],
       "g_s3": ["s3", "q1", "q2", "q3"], "g_gate": ["a34", "lb34"],
       "g_s4": ["s4"], "g_s5": ["s5", "s5n"], "g_ret": ["a56", "lb56", "s6"], "g_bn": ["bn"]},
      [("c5_full", []),
       ("c5_f1", ["g_s1"]), ("c5_f2", ["g_s2"]), ("c5_f3", ["g_s3"]), ("c5_f4", ["g_gate"]),
       ("c5_f5", ["g_s4"]), ("c5_f6", ["g_s5"]), ("c5_f7", ["g_ret"]), ("c5_f8", ["g_bn"])])

# ================= C6 批改账本卡（+底账条——全片最大证据物） =================
c6_cells = [
    box("rc", "17 v2 回执：ackedSeq = 1301 · 权威位 = (1234.8, 629.0)", 70, 150, 1780, 80, "#e1f5fe", "#0288d1", 30),
    # 证据物：底账条（环形缓冲 1s×30）
    text("blabel", "底账 · 环形缓冲（1 秒 ×30 条，示意切片）", 70, 248, 560, 28, 20, bold=True),
    box("buf0", "seq 1300　✓ 已确认", 70, 282, 420, 66, "#d5e8d4", "#82b366", 22),
    box("buf1", "seq 1301　● 对账中", 510, 282, 420, 66, "#fff2cc", "#d6b656", 22, "fontStyle=1;"),
    box("buf2", "seq 1302　待确认", 950, 282, 420, 66, "#f5f5f5", "#999999", 22),
    box("buf3", "seq 1303　待确认", 1390, 282, 420, 66, "#f5f5f5", "#999999", 22),
    box("k1", "案例① seq1301 预测 (1234.5, 629.1)　→　err = 3.2cm ＜ 5cm&#10;✓ 无感通过——只更新服务器认知，和解次数不动",
        70, 385, 1780, 105, "#d5e8d4", "#82b366", 26),
    box("k2", "案例② seq1301 预测 (1290.3, 629.0)　→　err = 55.5cm&#10;✗ 摆回权威位 → 重放 1302、1303（重抄）→ 和解次数 +1 · HUD 可见",
        70, 505, 1780, 125, "#f8cecc", "#b85450", 26),
    box("k3", "案例③ 回执 ackedSeq = 1305，底账查无此号&#10;✗ 断档——全量回滚到最新权威位，重放 0 帧",
        70, 645, 1780, 105, "#f8cecc", "#b85450", 26, "dashed=1;"),
    banner("bn", "抄是抄了，批改完重抄一遍——卷面还是您的笔迹", 70, 785, 1780, 85, 30),
    text("ft", "案例坐标为示意；ε = 5cm（kReconcileEpsilonCm）、底账 1s×30（kInputHistoryTarget）为代码真实实现", 70, 920, 1780, 36, 20, "#999999"),
]
C6 = (page(c6_cells, "批改现场：拿确认号，翻底账", "第 4 期 · 思路卡 6 / 6"),
      {"g_rc": ["rc"], "g_buf": ["blabel", "buf0", "buf1", "buf2", "buf3"],
       "g_k1": ["k1"], "g_k2": ["k2"], "g_k3": ["k3"], "g_bn": ["bn"]},
      [("c6_f1", ["g_rc"]), ("c6_f1b", ["g_buf"]), ("c6_f2", ["g_k1"]), ("c6_f3", ["g_k2"]),
       ("c6_f4", ["g_k3"]), ("c6_f5", ["g_bn"])])

CARDS = [C1, C2, C3, C4, C5, C6]

# ================= 灯位状态机 =================

def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*?style=")([^"]*)(")' % re.escape(sid))
    m = pat.search(xml)
    assert m, f"id 不存在: {sid}"
    return pat.sub(lambda mm: mm.group(1) + fn(mm.group(2)) + mm.group(3), xml, count=1)

def make_state(src_xml, groups, cur_groups):
    """current=粗边保语义色 / past=常态 / future=压暗；cur 空表=全亮入场帧。"""
    names = list(groups.keys())
    future = names[max(names.index(g) for g in cur_groups) + 1:] if cur_groups else []
    xml = src_xml
    for g in cur_groups:
        for sid in groups[g]:
            def cur(st):
                if 'endArrow=' in st:
                    return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=6', st) if 'strokeWidth=' in st else st + ';strokeWidth=6'
                if 'rounded=' in st:
                    return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=5', st) if 'strokeWidth=' in st else st + ';strokeWidth=5'
                return st
            xml = sub_style(xml, sid, cur)
    for g in future:
        for sid in groups[g]:
            xml = sub_style(xml, sid, lambda st: st + ';opacity=35;textOpacity=35')
    return xml

def render(xml, name):
    tmp = ROOT / f"_tmp_{name}.drawio"
    png = OUT / f"{name}.png"
    tmp.write_text(xml, encoding="utf-8")
    for _ in range(3):
        before = png.stat().st_mtime if png.exists() else 0
        subprocess.run([DRAWIO, "-x", "-f", "png", "-s", SCALE, "-o", str(png), str(tmp)], capture_output=True)
        if png.exists() and png.stat().st_mtime > before:
            break
        time.sleep(1.2)
    else:
        raise RuntimeError(f"渲染失败: {name}")
    tmp.unlink()
    print(f"  {png.name}  {png.stat().st_size//1024}KB")

for src, groups, states in CARDS:
    print(f"{states[0][0].split('_')[0]}:")
    for name, cur in states:
        render(make_state(src, groups, cur), name)
print("done:", len(list(OUT.glob('*.png'))), "frames")
