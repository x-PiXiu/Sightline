# -*- coding: utf-8 -*-
"""第 8 期思路卡产线：6 张 16:9 卡（1920×1080，scale2 渲染）——ep04 产线同款风格
设计律（沿 ep04 三条 + 本期主题"不停服"）：
  ①一卡一主题，卡标题=结论句；②证据物纪律：每个论点配可核验的实物
  （lua 片段 / 命令行 / 日志行 / 协议字节条）；③数字全部来自实测日志（evidence/）。
"""
import pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep08")
OUT = ROOT / "cards"
OUT.mkdir(exist_ok=True)
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

def code(i, v, x, y, w, h, fs=20):
    return (f'<mxCell id="{i}" value="{v}" style="rounded=0;whiteSpace=wrap;fillColor=#2b2b2b;'
            f'strokeColor=#555555;fontSize={fs};fontColor=#e8e8e8;fontFamily=Courier New;'
            f'align=left;spacing=8;{SK}" vertex="1" parent="1">'
            f'<mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def arrow(i, x1, y1, x2, y2, stroke="#666666", width=3, dashed=False):
    d = "dashed=1;dashPattern=8 4;" if dashed else ""
    return (f'<mxCell id="{i}" value="" style="edgeStyle=none;strokeColor={stroke};strokeWidth={width};'
            f'endArrow=block;endFill=1;{d}" edge="1" parent="1">'
            f'<mxGeometry relative="1" as="geometry"><mxPoint x="{x1}" y="{y1}" as="sourcePoint"/>'
            f'<mxPoint x="{x2}" y="{y2}" as="targetPoint"/></mxGeometry></mxCell>')

def banner(i, v, x, y, w, h, fs=32):
    return (f'<mxCell id="{i}" value="{v}" style="rounded=0;whiteSpace=wrap;fillColor=#f7f4ee;'
            f'strokeColor=#c8161d;strokeWidth=2;fontSize={fs};fontStyle=1;align=center;spacing=12;{SK}"'
            f' vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

def page(cells, title, stamp):
    bg = ('<mxCell id="bg" value="" style="rounded=0;fillColor=#ffffff;strokeColor=none;" '
          'vertex="1" parent="1"><mxGeometry x="0" y="0" width="1920" height="1080" as="geometry"/></mxCell>')
    body = "\n        ".join([bg, text("title", title, 70, 52, 1400, 60, 44, bold=True),
                             text("stamp", stamp, 1500, 60, 350, 40, 20, "#999999", "right")] + cells)
    return ('<mxfile host="app.diagrams.net">\n  <diagram name="cards" id="cards">\n'
            '    <mxGraphModel dx="1500" dy="900" grid="0" page="1" pageWidth="1920" pageHeight="1080" math="0" shadow="0">\n'
            '      <root>\n        <mxCell id="0"/><mxCell id="1" parent="0"/>\n        ' + body +
            '\n      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n')

def write(name, content):
    (OUT / f"{name}.drawio").write_text(content, encoding="utf-8")
    print(f"  ✓ {name}.drawio")

# ================= C1 热重载卡 =================
c1 = [
    text("h1", "改文件 ≠ 生效", 70, 150, 820, 44, 26, bold=True),
    text("h2", "一条命令 = 生效", 1030, 150, 820, 44, 26, bold=True),
    code("e1", "game_config.lua&#10;rifle = { damage = 25, ... }&#10;→ 改成 damage = 15&#10;（磁盘已变，内存没变）", 70, 210, 820, 170),
    box("x1", "服务器内存里的武器表：还是 25", 70, 400, 820, 90, "#f8cecc", "#b85450", 24),
    code("c1", "> reload 削步枪", 1030, 210, 820, 60),
    box("c2", "武器表：25 → 15　（下一发就按新表判）", 1030, 290, 820, 90, "#d5e8d4", "#82b366", 24),
    code("c3", "[配置] reload 成功 v2-680722bc", 1030, 400, 820, 60, 20),
    text("m1", "同一条命中判定链路，读的只是内存表——&#10;磁盘和内存之间，隔着一个命令", 70, 520, 1780, 70, 24, "#666666"),
    box("w1", "rollback 一键回上一版", 70, 620, 560, 90, "#fff2cc", "#d6b656", 24),
    box("w2", "改坏了？自动拒收保留旧表", 680, 620, 560, 90, "#fff2cc", "#d6b656", 24),
    box("w3", "审计链落 MySQL（谁改的/改了啥）", 1290, 620, 560, 90, "#fff2cc", "#d6b656", 24),
    banner("bn", "改数值像改音量——不用停服，下一发生效", 70, 760, 1780, 100, 34),
    text("ft", "实测：reload 回包毫秒级 · 证据 docs/episodes/ep08/evidence/ · 契约 v4.8", 70, 910, 1780, 36, 22, "#999999"),
]
write("ep08_c1_hotreload", page(c1, "改数值像改音量", "第 8 期 · 思路卡 1 / 6"))

# ================= C2 patch 热修卡 =================
c2 = [
    text("h1", "改数值不够呢？", 70, 150, 820, 44, 26, bold=True),
    text("h2", "整个函数都能换", 1030, 150, 820, 44, 26, bold=True),
    code("o1", "damage.lua（旧逻辑）&#10;local dmg = base&#10;if ctx.dist > 3000 then&#10;  dmg = dmg * 0.8 end&#10;return math.max(0, hp - dmg)", 70, 210, 820, 190),
    arrow("p1", 480, 420, 480, 470, "#c8161d", 4),
    code("cmd1", "> patch damage", 70, 480, 820, 60, 20),
    code("n1", "damage.lua（新逻辑）&#10;local dmg = base * 0.5&#10;return math.max(0, hp - dmg)", 1030, 210, 820, 190),
    arrow("p2", 1440, 420, 1440, 470, "#82b366", 4),
    box("r1", "Lua 引擎：换上新的函数引用&#10;上一版闭包交给 GC——不重启", 70, 490, 1780, 90, "#e1f5fe", "#0288d1", 24),
    box("s1", "坏脚本？pcall 拦截 → 自动回退内建公式", 70, 610, 860, 90, "#fff2cc", "#d6b656", 24),
    box("s2", "人机决策 bot_ai.lua 同款热修", 1030, 610, 820, 90, "#fff2cc", "#d6b656", 24),
    banner("bn", "数值用 reload，逻辑用 patch——都不用停服", 70, 750, 1780, 100, 34),
    text("ft", "沙箱：摘除 os/io · 32MB 内存上限 · 返回值校验 · 实测 evidence/srv_cmdport.log", 70, 900, 1780, 36, 22, "#999999"),
]
write("ep08_c2_patch", page(c2, "公式也能换零件", "第 8 期 · 思路卡 2 / 6"))

# ================= C3 三库选型卡 =================
c3 = [
    box("a1", "战绩 + 配置审计链&#10;要稳、要查、要对账", 70, 180, 560, 240, "#d5e8d4", "#82b366", 26),
    box("a1t", "MySQL · 行存储", 70, 150, 560, 1, fs=1),
    text("a1l", "MySQL 行存储", 70, 128, 560, 44, 28, bold=True),
    box("b1", "一局一文档战报&#10;名册 + 击杀时间线", 680, 180, 560, 240, "#e1f5fe", "#0288d1", 26),
    text("b1l", "MongoDB 文档", 680, 128, 560, 44, 28, bold=True),
    box("c1", "击杀热榜投影&#10;要快、丢了能重建", 1290, 180, 560, 240, "#fff2cc", "#d6b656", 26),
    text("c1l", "Redis 热榜", 1290, 128, 560, 44, 28, bold=True),
    text("lab", "按数据特性选型——不按名气选型", 70, 460, 1780, 50, 28, bold=True),
    box("wal", "审计 WAL：任一库故障期间，审计数据先落本地文件；恢复后回放补写&#10;——&#34;审计一条不丢&#34;与&#34;对局零中断&#34;同时成立", 70, 540, 1780, 130, "#f7f4ee", "#c8161d", 26),
    box("dual", "lite / full 双模式：无库裸跑与三库满配，同一份业务代码——协议输出逐字节一致（对拍实测）", 70, 700, 1780, 90, "#e1f5fe", "#0288d1", 26),
    banner("bn", "库是外包的内存——挂了不影响打架，账一条不丢", 70, 830, 1780, 100, 34),
    text("ft", "实测：杀 redis/mongo/mysql 对局照跑 · WAL 回放 2 条补写 · 证据 docs/episodes/ep08/evidence/", 70, 970, 1780, 36, 22, "#999999"),
]
write("ep08_c3_storage", page(c3, "数据分流，各回各家", "第 8 期 · 思路卡 3 / 6"))

# ================= C4 双模式卡 =================
c4 = [
    text("h1", "lite：无库裸跑", 250, 150, 600, 44, 28, bold=True, align="center"),
    text("h2", "full：三库满配", 1100, 150, 600, 44, 28, bold=True, align="center"),
    box("l1", "无 MySQL / Redis / MongoDB&#10;审计落本地 WAL 文件", 100, 230, 700, 130, fs=24),
    box("r1", "三库全挂——审计落 WAL、恢复补写&#10;任一库挂——降级计数、对局照跑", 1050, 230, 780, 130, fs=24),
    box("same", "同一份业务代码　同一套协议　同一条命中判定链", 100, 400, 1730, 90, "#e1f5fe", "#0288d1", 28),
    text("q", "怎么证明两种模式行为一致？", 100, 530, 1730, 50, 28, bold=True),
    code("diff", "$ wire_ab_test.sh&#10;══ diff（空 = 对拍通过）══&#10;IDENTICAL ✓", 100, 610, 1730, 150, 22),
    banner("bn", "库是外包的内存——挂了不影响打架，账一条不丢", 70, 800, 1780, 100, 34),
    text("ft", "对拍实测：同输入序列 17/21/30 号逐字节一致 · 证据 docs/episodes/ep08/evidence/ · 契约 v4.8", 70, 950, 1780, 36, 22, "#999999"),
]
write("ep08_c4_dualmode", page(c4, "一份代码，两种穿法", "第 8 期 · 思路卡 4 / 6"))

# ================= C5 命令口卡 =================
c5 = [
    text("h1", "运维命令口：与日志流分离", 70, 150, 1780, 44, 26, bold=True),
    box("w1", "CLion / 服务器日志窗口&#10;（心跳刷屏——观感嘈杂）", 100, 230, 800, 150, "#f8cecc", "#b85450", 24),
    box("w2", "nc 127.0.0.1 18889&#10;（命令窗口——干净输入）", 1050, 230, 800, 150, "#d5e8d4", "#82b366", 24),
    arrow("f1", 1450, 305, 1450, 420, "#82b366", 4),
    box("hub", "命令入队 → 编排器逐条处理 → 回执广播回命令窗口", 100, 430, 1730, 90, "#e1f5fe", "#0288d1", 26),
    code("out", "> version            → version: v1-680722bc&#10;> reload 削步枪      → reload 成功 v2-680722bc&#10;> rollback           → 内存回退上一版", 100, 550, 1730, 170, 22),
    box("adm", "这些能力通常是运营平台做的——这里做进了服务器本体", 100, 760, 1730, 90, "#fff2cc", "#d6b656", 26),
    banner("bn", "命令与响应同窗——拍摄时这就是闭环画面", 70, 890, 1780, 100, 34),
    text("ft", "实测：nc 敲 version/reload → 同窗回显 · 证据 docs/episodes/ep08/evidence/srv_cmdport.log", 70, 1030, 1780, 36, 22, "#999999"),
]
write("ep08_c5_cmdport", page(c5, "命令与响应，同窗闭环", "第 8 期 · 思路卡 5 / 6"))

# ================= C6 多房边界卡 =================
c6 = [
    text("h1", "压测发现一堵墙", 70, 150, 1780, 44, 26, bold=True),
    box("b1", "100 房", 130, 300, 240, 120, fs=24),
    box("b2", "250 房", 420, 300, 240, 120, fs=24),
    box("b3", "500 房", 710, 300, 240, 120, fs=24),
    box("b4", "1000 房", 1000, 300, 240, 120, fs=24),
    box("b5", "2000 房", 1290, 300, 240, 120, fs=24),
    text("s1", "0.1 帧/s", 130, 440, 240, 36, 26, "#c8161d", "center", bold=True),
    text("s2", "0.1 帧/s", 420, 440, 240, 36, 26, "#c8161d", "center", bold=True),
    text("s3", "0.1 帧/s", 710, 440, 240, 36, 26, "#c8161d", "center", bold=True),
    text("s4", "0.1 帧/s", 1000, 440, 240, 36, 26, "#c8161d", "center", bold=True),
    text("s5", "0.1 帧/s", 1290, 440, 240, 36, 26, "#c8161d", "center", bold=True),
    text("ref", "应≈30 帧/s（单房实测）", 130, 500, 1400, 36, 22, "#666666"),
    box("why", "根因：对局服务是「常驻单局」设计——第 N 次 StartMatch 会重置对局。&#10;多房压测下，快照只属于最后一个房——其余全部饿死。&#10;这不是性能极限，是架构边界。", 70, 560, 1780, 150, "#f7f4ee", "#c8161d", 26),
    banner("bn", "先量化，再拆除——第 9 期拆的就是这堵墙", 70, 750, 1780, 100, 34),
    text("ft", "压测：100~2000 房梯度 · 快照速率 p50=0.1（应≈30）· 证据 docs/episodes/ep08/evidence/", 70, 900, 1780, 36, 22, "#999999"),
]
write("ep08_c6_boundary", page(c6, "一堵墙，先量再拆", "第 8 期 · 思路卡 6 / 6"))

print("6 卡生成完成")
