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

# ================= C7 热更新三步流程卡（S2-01 主力画面·领导版原理锚） =================
c7 = [
    text("lead", "热更新 = 三个动作 —— 每一步都有保险", 70, 148, 1780, 46, 30, bold=True),
    # 三格横排
    box("s1", "① 读新的&#10;&#10;把配置/脚本文件&#10;重新加载进内存", 70, 230, 540, 250, "#e1f5fe", "#0288d1", 27),
    box("s2", "② 验合格&#10;&#10;格式 + 数值范围校验&#10;坏数据进不了门", 690, 230, 540, 250, "#fff2cc", "#d6b656", 27),
    box("s3", "③ 换上去&#10;&#10;原子切换&#10;下一帧起全读新值", 1310, 230, 540, 250, "#d5e8d4", "#82b366", 27),
    arrow("a1", 615, 355, 685, 355, "#666666", 5),
    arrow("a2", 1235, 355, 1305, 355, "#666666", 5),
    # 保险行（每格正下方）
    box("f1", "保险：读失败 → 旧表原样保留", 70, 500, 540, 66, "#f8fbff", "#6c8ebf", 22),
    box("f2", "保险：校验不过 → 拒收，旧版继续跑", 690, 500, 540, 66, "#f8fbff", "#6c8ebf", 22),
    box("f3", "保险：瞬间完成 —— 没有「改到一半」的中间态", 1310, 500, 540, 66, "#f8fbff", "#6c8ebf", 22),
    # 两层对象（比喻）
    text("tl", "两层对象，两条命令", 70, 618, 1780, 40, 26, bold=True),
    box("d1", "数据（武器数值/规则/人机参数）&#10;→ reload 换表 · 下一发生效&#10;比喻：换弹匣", 70, 674, 860, 150, "#f7f4ee", "#c8161d", 25),
    box("d2", "逻辑（伤害公式 / 人机决策函数）&#10;→ patch 换引用 · 旧闭包交垃圾回收&#10;比喻：换零件（机器不停）", 980, 674, 870, 150, "#f7f4ee", "#c8161d", 25),
    banner("bn", "改崩了？自动回退上一版 —— 笼子里的计算，出不去", 70, 866, 1780, 100, 32),
    text("ft", "沙箱：摘除 os/io · 32MB 内存上限 · 返回值域校验 · 实测 evidence/srv_cmdport.log", 70, 1000, 1780, 36, 22, "#999999"),
]
write("ep08_c7_hotupdate_flow", page(c7, "读新的、验合格、换上去", "第 8 期 · 思路卡 7 / 7"))

# ═══ 内存变化分帧系列 m1~m6（S2 口播逐帧·取代 anim_mem08 动画）═══

# ================= M1 启动定格（内存地图全景） =================
m1 = [
    text("h1", "配置区", 70, 150, 700, 40, 28, bold=True),
    box("cfg1", "cfg_ ──▶  [ v1 表 ]", 70, 200, 700, 70, "#e1f5fe", "#0288d1", 26),
    box("v1a", "伤害 25 · 弹匣 30 · 换弹 2500ms · 射速 95ms&#10;规则：血量 100 · 重生 3s · 十杀判胜&#10;人机：反应 250ms · 瞄准误差 4°", 70, 300, 700, 170, "#f8fbff", "#6c8ebf", 24),
    text("h2", "快照环（rollback 版本历史）", 70, 500, 700, 34, 22, "#666666"),
    box("rg1", "[ v1 ]   [ 空 ]   [ 空 ]   [ 空 ]", 70, 544, 700, 64, "#fff9e6", "#d6b656", 26),
    text("h3", "Lua VM 区（常驻 · 活到关服）", 850, 150, 560, 40, 28, bold=True),
    box("lua1", "refDamage ──▶ [ 闭包 A（伤害公式） ]&#10;refBot　　 ──▶ [ 闭包 B（人机决策） ]", 850, 200, 560, 130, "#f8fbff", "#6c8ebf", 24),
    text("h4", "客户端（30 号对端）", 850, 370, 560, 34, 22, "#666666"),
    box("cli1", "RuntimeParams：武器[0] 伤害 = 25", 850, 414, 560, 64, "#f5f5f5", "#999999", 24),
    text("h5", "持久化（full 模式）", 70, 660, 700, 34, 22, "#666666"),
    box("db1", "MySQL ✓　Redis ✓　Mongo ✓　　AuditWal 就绪", 70, 704, 700, 64, "#f5f5f5", "#999999", 24),
    banner("bn", "启动 = 规则定格成内存结构 —— 它不动，游戏就不变", 70, 830, 1780, 100, 32),
    text("ft", "数值=game_config.lua 实值 · evidence/ · 对应口播 S2-01", 70, 970, 1780, 36, 22, "#999999"),
]
write("ep08_m1_memstart", page(m1, "启动定格：内存里的三块", "第 8 期 · 内存帧 1 / 6"))

# ================= M2 对局中读表 =================
m2 = [
    text("h1", "对局中 · 每发子弹读这张表", 70, 150, 1780, 44, 30, bold=True),
    box("fire1", "开火请求", 70, 330, 240, 90, "#fff2cc", "#d6b656", 26),
    arrow("fa1", 330, 375, 560, 375, "#d6b656", 5),
    box("tab1", "[ v1 表 ]&#10;伤害 25", 570, 260, 420, 220, "#e1f5fe", "#0288d1", 30),
    arrow("fa2", 1010, 375, 1240, 375, "#82b366", 5),
    box("hit1", "弹着点&#10;飘字 伤害=25", 1250, 300, 420, 150, "#d5e8d4", "#82b366", 28),
    box("note1", "消费者 MatchService.weaponTable_ = v1 的值拷贝——&#10;每一发判定读它，读到的永远是同一张定格的表", 70, 560, 1780, 120, "#f7f4ee", "#c8161d", 26),
    banner("bn", "表不动 → 游戏不变（热更前的常态）", 70, 740, 1780, 100, 34),
    text("ft", "对应口播 S2-01/实测：判定日志逐发 evidence/", 70, 890, 1780, 36, 22, "#999999"),
]
write("ep08_m2_readtable", page(m2, "对局中：每发读表", "第 8 期 · 内存帧 2 / 6"))

# ================= M3 旁建+验（reload 执行中） =================
m3 = [
    text("h1", "reload 执行中 · 旁建 + 验", 70, 150, 1780, 44, 30, bold=True),
    box("cmd3", "> reload 削步枪", 70, 220, 500, 66, "#2b2b2b", "#555555", 26, "fontColor=#e8e8e8;"),
    box("v1b", "[ v1 表 ]&#10;伤害 25 —— 照常服务，一根毛没动", 70, 340, 780, 130, "#e1f5fe", "#0288d1", 26),
    box("v2b", "[ v2 表 ]（旁边新建）&#10;伤害 15 ← 正在校验：格式 + 数值范围", 920, 340, 780, 130, "#d5e8d4", "#82b366", 26),
    arrow("ok3", 1310, 300, 1310, 336, "#82b366", 4),
    text("okt", "校验 ✓", 1330, 296, 200, 34, 26, "#1f6b30", bold=True),
    box("bad3", "验不过？→ v2 当场销毁，v1 原样继续跑——坏数据进不了门", 70, 520, 1630, 90, "#f8cecc", "#b85450", 26),
    banner("bn", "新表在旁边建好验过，才轮到换 —— 旧的全程无感", 70, 700, 1780, 100, 34),
    text("ft", "实测：坏表拒收回执 evidence/srv_cmdport.log · 对应口播 S2-02", 70, 850, 1780, 36, 22, "#999999"),
]
write("ep08_m3_buildverify", page(m3, "旁建 + 验：旧表一根毛不动", "第 8 期 · 内存帧 3 / 6"))

# ================= M4 指针一跳 + v1 归档 =================
m4 = [
    text("h1", "换上去 · 指针一跳", 70, 150, 1780, 44, 30, bold=True),
    box("cfg4", "cfg_ ──▶  [ v2 表 ]（伤害 15）", 70, 220, 900, 70, "#d5e8d4", "#82b366", 28),
    text("flash", "⚡ 跳变瞬间：任何一帧，要么全旧 —— 要么全新", 70, 320, 900, 44, 26, "#c8161d", bold=True),
    box("w4", "weaponTable_ = 伤害 15（下一发判定读到新值）", 70, 400, 900, 70, "#e1f5fe", "#0288d1", 26),
    text("h4b", "v1 没死 —— 归档进快照环（rollback 的底气）", 1030, 210, 820, 40, 26, bold=True),
    box("rg4", "[ v2 ]   [ v1 ]   [ 空 ]   [ 空 ]", 1030, 270, 820, 74, "#fff9e6", "#d6b656", 28),
    box("rb4", "rollback = 指针从环里拨回上一版（一键回滚）", 1030, 390, 820, 70, "#fff2cc", "#d6b656", 25),
    box("net4", "同一跳变同时广播 30 号 → 客户端 RuntimeParams 刷新 → 飘字 25→15", 70, 540, 1780, 80, "#f7f4ee", "#c8161d", 26),
    banner("bn", "原子切换 —— 没有「改到一半」的中间态", 70, 700, 1780, 100, 34),
    text("ft", "实测：reload 回包 v1→v2 · evidence/srv_cmdport.log · 对应口播 S2-03", 70, 850, 1780, 36, 22, "#999999"),
]
write("ep08_m4_jump", page(m4, "指针一跳：全旧或全新", "第 8 期 · 内存帧 4 / 6"))

# ================= M5 跨进程回声 =================
m5 = [
    text("h1", "内存跳变的回声 · 30 号跨进程", 70, 150, 1780, 44, 30, bold=True),
    box("srv5", "服务器内存&#10;&#10;cfg_ ──▶ [ v2 表 ]（伤害 15）&#10;weaponTable_ = 15", 70, 240, 740, 200, "#d5e8d4", "#82b366", 26),
    arrow("msg5", 830, 340, 1140, 340, "#1f6feb", 6),
    text("msgt", "30 号 · 配置 v2 · 武器[0] 伤害=15", 845, 300, 320, 36, 22, "#1f6feb", "center", bold=True),
    box("cli5", "客户端内存&#10;&#10;RuntimeParams：武器[0] 伤害 25 → 15&#10;HUD 飘字：25 → 15", 1150, 240, 700, 200, "#f5f5f5", "#999999", 26),
    box("note5", "服务器内存里的一次指针跳变，变成您屏幕上掉血的变化——&#10;改表不是黑盒：每一跳都有回声，回声可对账（日志 → MySQL → HUD 版本号）", 70, 520, 1780, 130, "#f7f4ee", "#c8161d", 26),
    banner("bn", "您看到的掉血变了 —— 就是这次跳变的回声", 70, 730, 1780, 100, 34),
    text("ft", "实测：30 号广播+HUD 版本行 · 对应口播 S2-03", 70, 880, 1780, 36, 22, "#999999"),
]
write("ep08_m5_echo", page(m5, "一次跳变，两台机器", "第 8 期 · 内存帧 5 / 6"))

# ================= M6 patch 换引用 + 反例 =================
m6 = [
    text("h1", "逻辑热修 · 换引用不换进程", 70, 150, 1780, 44, 30, bold=True),
    box("p1", "patch 前：refDamage ──▶ [ 闭包 A（旧公式） ]", 70, 220, 830, 70, "#f8fbff", "#6c8ebf", 26),
    box("p2", "patch 后：refDamage ──▶ [ 闭包 A′（新公式） ]&#10;闭包 A 没人再引用 → ♻ 垃圾回收自己收走", 70, 330, 830, 120, "#d5e8d4", "#82b366", 26),
    box("p3", "Lua VM 不重启——换的只是 registry 里的一个引用&#10;下一发伤害判定取到的就是新函数", 70, 490, 830, 100, "#e1f5fe", "#0288d1", 25),
    text("h6b", "反例：改崩了怎么办", 960, 210, 780, 40, 28, bold=True),
    box("x1", "新脚本编译 + 出口校验（返回值域）", 960, 264, 780, 64, "#fff2cc", "#d6b656", 25),
    box("x2", "校验 ✗ → A′ 出栈即弃，refDamage 纹丝没动", 960, 356, 780, 64, "#f8cecc", "#b85450", 25),
    box("x3", "判定继续调 A —— 旧公式照跑，服务器不塌", 960, 448, 780, 64, "#d5e8d4", "#82b366", 25),
    banner("bn", "改崩了，引用根本没拨 —— 旧的照跑", 70, 660, 1780, 100, 34),
    text("ft", "沙箱：摘 os/io · 32MB 上限 · 返回值域校验 · 实测 evidence/srv_cmdport.log · 对应口播 S2-04/05", 70, 810, 1780, 36, 22, "#999999"),
]
write("ep08_m6_patchref", page(m6, "换引用不换进程", "第 8 期 · 内存帧 6 / 6"))

print("7 卡 + 6 内存帧生成完成")
