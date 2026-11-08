# -*- coding: utf-8 -*-
"""第 8 期架构图：ep08 新增组件（红描边高亮）叠在既有拓扑上——1920×1080"""
import pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep08")
OUT = ROOT / "arch"
OUT.mkdir(exist_ok=True)
SK = "sketch=1;curveFitting=1;jiggle=2;"
NEW = "strokeColor=#c8161d;strokeWidth=3;"
OLD = "strokeColor=#999999;strokeWidth=1.5;"

def box(i, v, x, y, w, h, fill="#ffffff", stroke="#6c8ebf", fs=24, extra=""):
    return (f'<mxCell id="{i}" value="{v}" style="rounded=1;whiteSpace=wrap;fillColor={fill};'
            f'strokeColor={stroke};fontSize={fs};{SK}{extra}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def text(i, v, x, y, w, h, fs=24, color="#333333", align="left", bold=False):
    b = "fontStyle=1;" if bold else ""
    return (f'<mxCell id="{i}" value="{v}" style="text;fontSize={fs};{b}fontColor={color};'
            f'align={align};whiteSpace=wrap;" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def arrow(i, x1, y1, x2, y2, stroke="#666666", width=3, dashed=False, label=""):
    d = "dashed=1;dashPattern=8 4;" if dashed else ""
    lb = f'value="{label};' if label else 'value="'
    return (f'<mxCell id="{i}" {lb}" style="edgeStyle=none;strokeColor={stroke};strokeWidth={width};'
            f'endArrow=block;endFill=1;fontSize=20;{d}" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/></mxGeometry></mxCell>')

def page(cells, title, stamp):
    bg = ('<mxCell id="bg" value="" style="rounded=0;fillColor=#ffffff;strokeColor=none;" '
          'vertex="1" parent="1"><mxGeometry x="0" y="0" width="1920" height="1080" as="geometry"/></mxCell>')
    body = "\n        ".join([bg, text("title", title, 70, 40, 1500, 56, 40, bold=True),
                             text("stamp", stamp, 1520, 50, 330, 40, 20, "#999999", "right")] + cells)
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="arch" id="arch">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        ' + body +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')

cells = [
    # ── 既有层（灰）──
    box("net", "epoll 主从 Reactor&#10;（IO 多线程 · 拆帧 · 回投）", 100, 200, 480, 110, "#f5f5f5", "#999999", 24),
    box("orch", "ApplicationOrchestrator&#10;帧节律 · 命令队列 · 事件扇出 · 生命周期", 100, 420, 480, 130, "#f5f5f5", "#999999", 24),
    box("biz", "业务层：LobbyService + MatchService&#10;（房间 / 对局 / 权威判定 · 零锁）", 100, 650, 480, 130, "#f5f5f5", "#999999", 24),
    box("client", "UE5 客户端（预测/和解/插值）", 150, 850, 380, 90, "#f5f5f5", "#999999", 24),
    arrow("a_nc", 340, 310, 340, 420, "#999999"),
    arrow("a_cb", 340, 550, 340, 650, "#999999"),
    arrow("a_bc", 400, 780, 400, 850, "#999999"),
    # ── ep08 新增（红）──
    box("cfg", "ConfigService&#10;game_config.lua 热重载 / rollback&#10;四视图注入（G9 ISP）", 700, 130, 460, 140, "#fff2cc", "#c8161d", 22),
    box("lua", "LuaRuntime&#10;damage.lua 公式热修&#10;bot_ai.lua 决策热修&#10;沙箱（禁 os/io · 32MB）", 1250, 130, 460, 180, "#fff2cc", "#c8161d", 22),
    box("cmd", "运维命令口 :18889&#10;nc → 命令队列 → 回执回写", 700, 420, 460, 120, "#fff2cc", "#c8161d", 22),
    box("dual", "lite / full 双模式&#10;无库裸跑 ⇄ 三库满配（逐字节一致）", 1250, 420, 460, 120, "#fff2cc", "#c8161d", 22),
    box("per", "persistence 双队列&#10;审计队列（WAL 兜底）+ 统计队列", 700, 620, 460, 130, "#fff2cc", "#c8161d", 22),
    box("my", "MySQL&#10;战绩+审计链", 1250, 590, 200, 90, "#d5e8d4", "#c8161d", 20),
    box("mg", "MongoDB&#10;一局一文档战报", 1465, 590, 200, 90, "#e1f5fe", "#c8161d", 20),
    box("rd", "Redis&#10;热榜+登录态", 1250, 720, 200, 90, "#fff2cc", "#c8161d", 20),
    box("wal", "AuditWal&#10;库故障落盘→补写", 1465, 720, 200, 90, "#f8cecc", "#c8161d", 20),
    # 箭头（新增链路）
    arrow("a_lc", 580, 265, 700, 210, "#c8161d", 3, False, "reload/rollback"),
    arrow("a_cl", 930, 480, 1250, 260, "#c8161d", 3, False, "patch"),
    arrow("a_oc", 580, 470, 700, 470, "#c8161d", 3, False, "命令入队"),
    arrow("a_or", 580, 700, 700, 680, "#c8161d", 3, False, "事件扇出"),
    arrow("a_pm", 1160, 670, 1250, 655, "#c8161d", 3),
    arrow("a_pd", 1160, 720, 1250, 745, "#c8161d", 3),
    arrow("a_mw", 1450, 680, 1465, 730, "#c8161d", 3, True, "故障降级"),
    # 图例
    box("lg", "红描边 = 第 8 期新增　　灰 = 既有（第 1~7 期）", 700, 880, 700, 60, "#f7f4ee", "#c8161d", 22),
    text("ft", "数字均实测：30000 长连接 · 587 断言 · 热更毫秒级 · WAL 回放一条不丢 · 证据 docs/episodes/ep08/evidence/", 100, 990, 1700, 36, 20, "#999999"),
]
content = page(cells, "Sightline · 不停服运维层（第 8 期架构）", "第 8 期 · 架构定稿")
(OUT / "ep08_server_architecture.drawio").write_text(content, encoding="utf-8")
print("  ✓ ep08_server_architecture.drawio")
