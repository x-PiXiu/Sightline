# -*- coding: utf-8 -*-
"""第 3 期服务端架构图走查灯位产线（a1~a5——S3-01/02 段，治 39s 静态图）
三态语义沿用 ep02 arch_walk / 本期 make_ep03_walkthrough：
  current = 亮红底 #ffcccc + 粗边（框 5 / 箭头 6）；past = 定稿常态；future = opacity35（含文字）
a5 额外注入「零 socket · 只认编号」注记（仅走查版，定稿图不带）。
坑沿用：唯一临时名渲染 + mtime 新鲜断言 + 重试。
"""
import re, subprocess, time, pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep03")
OUT = ROOT / "arch_walk"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"

# 组序 = 时间序；组间 past/future 由索引推导
GROUPS = {
    "g_match":  ["b_match", "eme"],
    "g_bcast":  ["b_bcast", "ebc", "ebclbl"],
    "g_wires":  ["edm", "emw", "emwlbl"],
    "g_lobby":  ["b_lobby", "lwlobby"],
}
STATES = [
    ("a1", ["g_match"]),
    ("a2", ["g_bcast"]),
    ("a3", ["g_wires"]),
    ("a4", ["g_lobby", "g_match"]),          # 两间屋：双框同时亮
    ("a5", ["g_wires"]),                     # 中间人抄名册 + 注记注入
]
NOTE_XML = ('<mxCell id="arch_zero_note" value="零 socket · 只认编号" '
            'style="rounded=1;whiteSpace=wrap;fillColor=#f8f0f0;strokeColor=#c8161d;'
            'strokeWidth=2;fontSize=13;fontStyle=1;fontColor=#c8161d;sketch=1;" '
            'vertex="1" parent="1"><mxGeometry x="760" y="601" width="205" height="26" '
            'as="geometry"/></mxCell>\n      ')


def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*?style=")([^"]*)(")' % re.escape(sid))
    assert pat.search(xml), f"id 不存在: {sid}"
    return pat.sub(lambda m: m.group(1) + fn(m.group(2)) + m.group(3), xml, count=1)


def make_state(src, cur_groups):
    names = list(GROUPS.keys())
    cur_ix = max(names.index(g) for g in cur_groups)
    future = names[cur_ix + 1:]
    xml = src
    for g in cur_groups:
        for sid in GROUPS[g]:
            def cur(st):
                if 'endArrow=' in st:
                    return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=6', st) if 'strokeWidth=' in st else st + ';strokeWidth=6'
                if 'rounded=' in st and 'fillColor=' in st:
                    st = re.sub(r'fillColor=#[0-9a-fA-F]+', 'fillColor=#ffcccc', st)
                    st = re.sub(r'strokeColor=#[0-9a-fA-F]+', 'strokeColor=#c8161d', st)   # 描边同换红
                    st = re.sub(r';?opacity=\d+', '', st)   # 剥离旧层暗化（ep02 层在 ep03 图是压暗件，灯位须全亮——judge 两轮实锤）
                    return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=5', st) if 'strokeWidth=' in st else st + ';strokeWidth=5'
                return st
            xml = sub_style(xml, sid, cur)
    for g in future:
        for sid in GROUPS[g]:
            xml = sub_style(xml, sid, lambda st: st + ';opacity=35;textOpacity=35')
    return xml


def render(xml, name):
    tmp = ROOT / f"_tmp_{name}.drawio"
    png = OUT / f"{name}.png"
    tmp.write_text(xml, encoding='utf-8')
    for _ in range(3):
        before = png.stat().st_mtime if png.exists() else 0
        subprocess.run([DRAWIO, "-x", "-f", "png", "-s", "2", "-o", str(png), str(tmp)], capture_output=True)
        if png.exists() and png.stat().st_mtime > before:
            break
        time.sleep(1.2)
    else:
        raise RuntimeError(f"渲染失败: {name}")
    tmp.unlink()
    print(f"  {png.name} {png.stat().st_size // 1024}KB")


src = (ROOT / "server_architecture.drawio").read_text(encoding='utf-8')
for name, cur in STATES:
    xml = make_state(src, cur)
    if name == "a5":
        # 零 socket 注记并入 b_match 框内文字（浮动注记三迁全压线——框自己说话=零碰撞）
        assert '开始对局（校验房主）' in xml
        xml = xml.replace('开始对局（校验房主）', '零 socket · 只认编号')
    render(xml, name)
print("done: 5")
