# -*- coding: utf-8 -*-
"""第 4 期架构图+输入之旅泳道产线 v2（1920×1080，scale2≈3842×2162）
产物：
  server_arch_ep04 / client_arch_ep04 —— 同 ep03 分层/配色/演进口径（灰=已有 橙红=新增 黄=唯一门）
  s7_full + s7_f1~f8 —— 「输入之旅」时序泳道（主角 seq 1303，八站灯位）
v2 修复（自审抓出）：①层带标题改独立文本格居左上（原 band 中央标题被盒子盖住）；
②长流线改 orthogonalEdgeStyle+边廊路由（edgeStyle=none 忽略路径点全走斜线穿盒）；
③流线全部避开盒子：左缘 x=62/78 · 层间空隙 y=265/694 · 右缘 x=1700/1760 · 缝隙 x=1245/960/970。
"""
import re, subprocess, time, pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep04")
OUT = ROOT / "arch"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"
SCALE = "2"
SK = "sketch=1;curveFitting=1;jiggle=2;"

def box(i, v, x, y, w, h, fill="#ffffff", stroke="#6c8ebf", fs=20, extra=""):
    return (f'<mxCell id="{i}" value="{v}" style="rounded=1;whiteSpace=wrap;fillColor={fill};'
            f'strokeColor={stroke};fontSize={fs};{SK}{extra}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def band(i, x, y, w, h, stroke="#6c8ebf", fill="#f8fbff"):
    return (f'<mxCell id="{i}" value="" style="rounded=1;whiteSpace=wrap;fillColor={fill};'
            f'strokeColor={stroke};strokeWidth=2;{SK}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def text(i, v, x, y, w, h, fs=22, color="#333333", align="left", bold=False):
    b = "fontStyle=1;" if bold else ""
    return (f'<mxCell id="{i}" value="{v}" style="text;fontSize={fs};{b}fontColor={color};'
            f'align={align};whiteSpace=wrap;" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def arrow(i, x1, y1, x2, y2, stroke="#666666", width=3, pts=None, dashed=False, orth=False):
    d = "dashed=1;dashPattern=8 4;" if dashed else ""
    es = "orthogonalEdgeStyle;" if orth else "edgeStyle=none;"
    p = f'<Array as="points">{"".join(f"<mxPoint x={a} y={b}/>" for a, b in pts)}</Array>' if pts else ""
    return (f'<mxCell id="{i}" value="" style="{es}strokeColor={stroke};strokeWidth={width};'
            f'endArrow=block;endFill=1;{d}" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/>{p}</mxGeometry></mxCell>')

def alabel(i, v, x, y, w, h, color="#333333", fs=17):
    return text(i, v, x, y, w, h, fs, color, "center", bold=True)


def flow(fid, pts, stroke="#666666", width=3):
    """多段直线拼折线：直线 100% 可渲染；最后一段带箭头。"""
    out = []
    for k in range(len(pts) - 1):
        (x1, y1), (x2, y2) = pts[k], pts[k + 1]
        last = (k == len(pts) - 2)
        out.append(arrow(f"{fid}_{k}", x1, y1, x2, y2, stroke, width) if last
                   else arrow(f"{fid}_{k}", x1, y1, x2, y2, stroke, 1))
    return out

def page(cells, title, stamp):
    bg = ('<mxCell id="bg" value="" style="rounded=0;fillColor=#ffffff;strokeColor=none;" '
          'vertex="1" parent="1"><mxGeometry x="0" y="0" width="1920" height="1080" as="geometry"/></mxCell>')
    body = "\n        ".join([bg, text("title", title, 50, 36, 1500, 54, 30, bold=True),
                             text("stamp", stamp, 1520, 44, 350, 36, 17, "#999999", "right")] + cells)
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="d" id="d">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        ' + body +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')

# ================= 服务端架构 ep04（v2：标题左上/边廊走线） =================
srv_cells = [
    band("bmain", 40, 100, 1840, 200),
    text("t_main", "main.cpp（装配根 · 单线程串行铁律不变）", 56, 110, 700, 26, 17, "#1f4e79", bold=True),
    box("m_consume", "帧队列消费&#10;frame_queue 统一取货 · 33ms 一批", 90, 170, 520, 80, "#fff2cc", "#d6b656", 19),
    box("m_snap", "实体快照广播&#10;17v2：权威位 + ackedSeq + 含自己", 660, 170, 560, 80, "#ffe6cc", "#c8161d", 19),
    box("m_hb", "心跳 echo&#10;校验 seq · 回 ACK", 1270, 170, 400, 80, fs=19),
    band("badap", 40, 320, 1840, 200),
    text("t_adap", "adapters（接口适配器）", 56, 330, 500, 26, 17, "#1f4e79", bold=True),
    box("a_codec", "FrameCodec 切帧&#10;（TryDecode 在 worker 侧执行）", 90, 390, 440, 80, fs=19),
    box("a_disp", "MessageDispatcher&#10;注册表 msgid → Parse（variant · 唯一 switch）", 580, 390, 560, 80, "#ffe6cc", "#c8161d", 19),
    box("a_fw", "FrameWriter&#10;领域事件 → 帧", 1190, 390, 440, 80, fs=19),
    band("bnet", 40, 580, 1840, 210, "#82b366", "#f6fbf4"),
    text("t_net", "net（底座升级：IO 线程化 · 唯一门 · 最小堆定时器）", 56, 590, 800, 26, 17, "#1f6b30", bold=True),
    box("n_acc", "Acceptor 接客 → selectWorker", 90, 622, 360, 66, fs=18),
    box("n_conn", "ClientConnection 连接", 480, 622, 340, 66, fs=18),
    box("n_buf", "Buffer 攒字节", 850, 622, 260, 66, fs=18),
    box("n_worker", "IO Worker ×N（新增）&#10;收字节 → TryDecode 切帧 → 投队列", 90, 706, 420, 76, "#ffe6cc", "#c8161d", 18),
    box("n_queue", "frame_queue 唯一跨线程门&#10;MPSC · Connected/Frame/Closed", 560, 706, 480, 76, "#fff2cc", "#d6b656", 18, "strokeWidth=3;"),
    box("n_heap", "EventLoop · HeapScheduler（改）&#10;纯最小堆定时器 · 实测拍距 33.0ms", 1090, 706, 500, 76, "#ffe6cc", "#c8161d", 18),
    band("bapp", 40, 840, 1840, 200),
    text("t_app", "application / domain（业务层 · 零改动抵押）", 56, 850, 700, 26, 17, "#1f4e79", bold=True),
    box("d_lobby", "LobbyService · 用例&#10;登录 / 建房 / 进房 / 列表", 90, 895, 400, 90, fs=18),
    box("d_match", "MatchService 权威推算&#10;上次权威位 + 钳制增量（上限 1 米/条）&#10;ackedSeq 只进不退", 530, 880, 600, 110, "#ffe6cc", "#c8161d", 18),
    box("d_entity", "Entity 权威位&#10;pos/yaw/pitch/flags&#10;+ ackedSeq", 1170, 895, 320, 90, "#ffe6cc", "#c8161d", 18),
    box("d_room", "Room · 实体&#10;成员≤4 · 房主=最早加入者", 1530, 895, 310, 90, fs=18),
    # 流线（多段直线拼折线，走廊坐标已核对无穿盒）
    *flow("sf1", [(840,706),(840,560),(50,560),(50,215),(90,215)], "#d6b656", 5),
    alabel("sf1l", "投递 · 跨线程 ×1（唯一门）", 120, 528, 280, 24, "#7f6000"),
    *flow("sf2", [(150,250),(150,305),(48,305),(48,880),(505,880),(505,935),(530,935)], "#c8161d", 4),
    alabel("sf2l", "tick 消费 · 分发 · 调用例", 60, 798, 260, 24, "#c8161d"),
    *flow("sf3", [(1130,935),(1170,935)], "#c8161d", 3),
    alabel("sf3l", "读写", 1112, 902, 60, 22, "#c8161d", 16),
    *flow("sf4", [(940,880),(940,826),(1700,826),(1700,145),(1160,145),(1160,170)], "#c8161d", 4),
    alabel("sf4l", "每 tick 组装 · 17v2", 1300, 798, 220, 24, "#c8161d"),
    *flow("sf5", [(1200,250),(1200,305),(1250,305),(1250,390)], "#c8161d", 3),
    *flow("sf6", [(1630,430),(1750,430),(1750,120),(1855,120)], "#6c8ebf", 4),
    alabel("sf6l", "17v2 回程&#10;→ 回到客户端", 1520, 96, 190, 46, "#1f4e79"),
    text("legend", "灰 = 第 1-3 期已有　橙红 = 第 4 期新增　黄 = 唯一跨线程门 | 19 号入站：Worker → frame_queue → tick 消费 · 17v2 出站：FrameWriter → outbound → EPOLLOUT · 业务层零改动 | 图 = 代码一致 · 契约 v4.1",
         50, 1046, 1840, 28, 15, "#555555"),
]

# ================= 客户端架构 ep04（v2） =================
cli_cells = [
    band("bmain", 40, 100, 1840, 160),
    text("t_main", "main.cpp（装配根）", 56, 110, 400, 26, 17, "#1f4e79", bold=True),
    box("m_conn", "连接管理 · 状态机", 90, 155, 420, 70, fs=19),
    box("m_hb", "心跳计时器 1s / seq", 540, 155, 340, 70, fs=19),
    box("m_disp", "帧分发 GameThread", 910, 155, 380, 70, fs=19),
    band("bgame", 40, 280, 1840, 350),
    text("t_game", "game（业务）—— 预测 · 和解 · 插值三线落地（本期主战场）", 56, 290, 800, 26, 17, "#1f4e79", bold=True),
    box("g_sess", "MatchSession 对局前台&#10;收 17v2 → 权威位 + ackedSeq → 对账", 90, 330, 400, 90, "#ffe6cc", "#c8161d", 18),
    box("g_reg", "EntityRegistry 世界账本&#10;PlayerId → EntityState", 90, 445, 400, 76, fs=18),
    box("g_spawn", "PawnSpawner 画面搬运&#10;Entity → Pawn", 90, 540, 400, 70, fs=18),
    box("g_local", "Character · 本地&#10;BS 移动当场生效&#10;+ 输入史环形缓冲 1s×30", 530, 330, 420, 100, "#ffe6cc", "#c8161d", 18),
    box("g_remote", "Character · 远端&#10;双快照插值 + 外推 200ms + 25ms 渲染延迟&#10;速度=Δack · 非对称低通", 530, 450, 420, 100, "#ffe6cc", "#c8161d", 18),
    box("g_rec", "SightlineReconcile 纯决策&#10;三分支：无感 / 回滚重放 / 全量兜底&#10;零引擎依赖 · 脱 UE 单测", 990, 330, 420, 100, "#ffe6cc", "#c8161d", 18, "strokeWidth=3;"),
    box("g_hud", "Debug HUD 三表&#10;预测误差 / 和解次数 / 重放帧数", 990, 450, 420, 76, "#ffe6cc", "#c8161d", 18),
    band("badap", 40, 650, 1840, 200),
    text("t_adap", "adapters / ue（引擎宿主）", 56, 660, 500, 26, 17, "#1f4e79", bold=True),
    box("ad_frame", "HandleFrame 逐帧校验长度", 90, 715, 400, 70, fs=18),
    box("ad_task", "AsyncTask(GameThread)&#10;接收线程唯一投递口", 530, 715, 400, 70, fs=18),
    box("ad_delay", "InboundDelay 滑块&#10;M5 损伤注入", 970, 715, 380, 70, fs=18),
    box("ad_apply", "ApplyState → Pawn&#10;+ AckedSeq 透传", 1390, 715, 400, 70, "#ffe6cc", "#c8161d", 18),
    band("bnet", 40, 870, 1840, 120, "#82b366", "#f6fbf4"),
    text("t_net", "net（复用底座）", 56, 880, 400, 26, 17, "#1f6b30", bold=True),
    box("n_conn", "FSightlineConnection 收发&#10;接收线程只搬字节 · 完整帧才投递", 90, 912, 620, 66, fs=18),
    box("n_gi", "UGameInstance 跨关卡不销毁", 760, 912, 460, 66, fs=18),
    # 流线（多段直线拼折线，走廊坐标已核对无穿盒）
    *flow("cf1", [(740,330),(740,320),(48,320),(48,945),(90,945)], "#c8161d", 4),
    alabel("cf1l", "① 19 号上行 · 30Hz（意图 + seq）", 56, 612, 320, 20, "#c8161d", 16),
    *flow("cf2", [(710,945),(730,945),(730,785)], "#6c8ebf", 3),
    alabel("cf2l", "② 完整帧投递", 745, 848, 180, 26, "#1f4e79"),
    *flow("cf2b", [(730,715),(730,640),(970,640),(970,225),(1050,225)], "#6c8ebf", 3),
    alabel("cf2bl", "②（续）转游戏线程", 600, 592, 220, 24, "#1f4e79", 16),
    *flow("cf3", [(910,225),(910,270),(290,270),(290,330)], "#6c8ebf", 3),
    alabel("cf3l", "③ 17v2 分发", 545, 276, 170, 26, "#1f4e79"),
    *flow("cf4", [(390,330),(390,310),(1200,310),(1200,330)], "#6c8ebf", 3),
    alabel("cf4l", "④ 权威位 + ackedSeq → 对账", 920, 288, 300, 26, "#1f4e79"),
    *flow("cf5", [(1410,380),(1450,380),(1450,660),(1590,660),(1590,715)], "#6c8ebf", 3),
    alabel("cf5l", "⑤ 修正 / 重放 → ApplyState", 1330, 672, 300, 26, "#1f4e79"),
    *flow("cf6", [(490,470),(530,470)], "#82b366", 3),
    alabel("cf6l", "⑥", 492, 438, 40, 22, "#1f6b30", 16),
    *flow("cf7", [(600,550),(600,640),(300,640),(300,610)], "#82b366", 3),
    alabel("cf7l", "⑦ 插值位姿", 370, 548, 160, 24, "#1f6b30", 16),
    text("legend", "灰 = 第 1-3 期已有　橙红 = 第 4 期新增 | 预测（本地当场动）· 和解（17v2 对账三分支）· 插值（远端双快照）三线并行 | 远端物理关闭 · 位置 100% 快照驱动 | 图 = 代码一致 · 契约 v4.1",
         50, 1000, 1840, 28, 15, "#555555"),
]

# ================= S7 输入之旅泳道（v1 已过审，不动） =================
L1cx, L2cx, L3cx, L4cx = 260, 720, 1150, 1620
s7_cells = [
    box("h1", "UE 客户端 · 手感侧", 80, 92, 360, 46, "#fff2cc", "#d6b656", 19, "fontStyle=1;"),
    box("h2", "IO Worker 线程", 540, 92, 360, 46, "#f6fbf4", "#82b366", 19, "fontStyle=1;"),
    box("h3", "frame_queue 唯一门", 980, 92, 340, 46, "#fff2cc", "#d6b656", 19, "fontStyle=1;strokeWidth=3;"),
    box("h4", "main 线程 · 权威侧", 1400, 92, 440, 46, "#e1f5fe", "#0288d1", 19, "fontStyle=1;"),
    arrow("l1", L1cx, 138, L1cx, 968, "#d6b656", 2, dashed=True),
    arrow("l2", L2cx, 138, L2cx, 968, "#82b366", 2, dashed=True),
    arrow("l3", L3cx, 138, L3cx, 968, "#d6b656", 4, dashed=True),
    arrow("l4", L4cx, 138, L4cx, 968, "#0288d1", 2, dashed=True),
    box("b1", "本地预测执行&#10;输入 → 当场动 · 入底账（seq,位置）×1s", 80, 170, 360, 90, "#fff2cc", "#d6b656", 18),
    arrow("m1", L1cx, 292, L2cx, 292, "#666666", 3),
    alabel("m1l", "19 PLAYER_INPUT{seq=1303} · 25B · 30Hz", 300, 262, 380, 24, "#333333"),
    box("b2", "收字节 → TryDecode 切帧", 540, 330, 360, 76, "#f6fbf4", "#82b366", 18),
    arrow("m2", L2cx, 438, L3cx, 438, "#d6b656", 5),
    alabel("m2l", "投递完整帧 · 跨线程 ×1", 790, 408, 280, 24, "#7f6000"),
    arrow("m2b", L3cx, 492, L4cx, 492, "#666666", 3),
    alabel("m2bl", "33ms tick 统一消费", 1190, 462, 260, 24, "#333333"),
    box("b3", "权威推算&#10;上次权威位 + 钳制增量（上限 1 米/条）· ackedSeq = 1303", 1400, 530, 440, 90, "#e1f5fe", "#0288d1", 18),
    box("b4", "快照组装&#10;17v2：权威位 + ackedSeq + 含自己", 1400, 655, 440, 80, "#ffe6cc", "#c8161d", 18),
    arrow("m3", L4cx, 768, L2cx, 768, "#6c8ebf", 4),
    alabel("m3l", "FrameWriter 编码 → outbound · 跨线程 ×2", 900, 738, 400, 24, "#1f4e79"),
    arrow("m4", L2cx, 830, L1cx, 830, "#82b366", 4),
    alabel("m4l", "17 v2 快照回程 —— 对账凭据", 330, 800, 320, 24, "#1f6b30"),
    box("bn", "一条输入进，一份快照出——门就这一扇，跨线程各一次", 80, 890, 1760, 60, "#f7f4ee", "#c8161d", 26, "fontStyle=1;"),
    text("ft", "主角 seq 1303 · 25B（契约 v4.1）| 路线 = main.cpp / match_service / EventLoop 实现 · 图 = 代码一致", 80, 972, 1760, 26, 15, "#999999"),
]
S7_GROUPS = {
    "g_up":   ["b1", "m1", "m1l"],
    "g_cut":  ["b2"],
    "g_door": ["m2", "m2l"],
    "g_tick": ["m2b", "m2bl"],
    "g_auth": ["b3"],
    "g_snap": ["b4"],
    "g_out":  ["m3", "m3l"],
    "g_back": ["m4", "m4l", "bn"],
}
S7_STATES = [
    ("s7_full", []),
    ("s7_f1", ["g_up"]), ("s7_f2", ["g_cut"]), ("s7_f3", ["g_door"]), ("s7_f4", ["g_tick"]),
    ("s7_f5", ["g_auth"]), ("s7_f6", ["g_snap"]), ("s7_f7", ["g_out"]), ("s7_f8", ["g_back"]),
]

# ================= 状态机+渲染 =================
def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*?style=")([^"]*)(")' % re.escape(sid))
    m = pat.search(xml)
    assert m, f"id 不存在: {sid}"
    return pat.sub(lambda mm: mm.group(1) + fn(mm.group(2)) + mm.group(3), xml, count=1)

def make_state(src_xml, groups, cur_groups):
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

import sys
only = sys.argv[1] if len(sys.argv) > 1 else "all"
if only in ("all", "arch"):
    render(page(srv_cells, "Sightline 服务端架构（第 4 期 · 权威与预测和解）—— 在第 3 期骨架上兑现演进预留", "图 = 代码一致 · 2026-10-22"), "server_arch_ep04")
    render(page(cli_cells, "Sightline 客户端架构（第 4 期 · 预测 · 和解 · 插值）—— 在第 3 期骨架上兑现演进预留", "图 = 代码一致 · 2026-10-22"), "client_arch_ep04")
if only in ("all", "s7"):
    s7_src = page(s7_cells, "第 4 期 · 输入之旅（时序泳道）—— 一条输入进，一份快照出", "定稿 v1 · 图 = 代码一致")
    print("s7:")
    for name, cur in S7_STATES:
        render(make_state(s7_src, S7_GROUPS, cur), name)
print("done")
