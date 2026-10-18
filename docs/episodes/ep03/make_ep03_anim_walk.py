# -*- coding: utf-8 -*-
"""第 3 期动画系统图走查产线 an_a1~a3（S4-08 详拍：数据源→ABP 四层→素材表+视角）
三态语义同 arch_walk：current=亮（框红底红边剥 opacity / 箭头 6 / 文字红粗）、past=常态、future=压暗35%。
坑沿用：唯一临时名+mtime 重试。
"""
import re, subprocess, time, pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep03")
OUT = ROOT / "anim_walk"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"

GROUPS = {   # 组序 = 口播讲序；表体 th_*/tr_* 运行时从源图正则抽取（表头表体必须同趟压暗——judge 实锤）
    "g_src":  ["conn", "src_net", "warn", "src_local", "char"],
    "g_abp":  ["abp_head", "L3", "L2", "L1", "L0"],
}
STATES = [("an_a1", ["g_src"]), ("an_a2", ["g_abp"]), ("an_a3", ["g_tail"])]


def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*?style=")([^"]*)(")' % re.escape(sid))
    assert pat.search(xml), f"id 不存在: {sid}"
    return pat.sub(lambda m: m.group(1) + fn(m.group(2)) + m.group(3), xml, count=1)


def make_state(src, cur_groups):
    names = [g for g in ORDER if g in GROUPS]
    cur_ix = max(names.index(g) for g in cur_groups)
    future = names[cur_ix + 1:]
    xml = src
    for g in cur_groups:
        for sid in GROUPS[g]:
            def cur(st):
                if 'endArrow=' in st:                                   # 箭头
                    return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=6', st) if 'strokeWidth=' in st else st + ';strokeWidth=6'
                if 'rounded=' in st and 'fillColor=' in st:             # 框：红底红边+剥暗化
                    st = re.sub(r'fillColor=#[0-9a-fA-F]+', 'fillColor=#ffcccc', st)
                    st = re.sub(r'strokeColor=#[0-9a-fA-F]+', 'strokeColor=#c8161d', st)
                    st = re.sub(r';?opacity=\d+', '', st)
                    return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=5', st) if 'strokeWidth=' in st else st + ';strokeWidth=5'
                if 'text;' in st:                                       # 文字：红粗焦点
                    st = re.sub(r'fontColor=#[0-9a-fA-F]+', 'fontColor=#c8161d', st)
                    return st + ';fontStyle=1'
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
        raise RuntimeError(name)
    tmp.unlink()
    print(f"  {png.name} {png.stat().st_size // 1024}KB")


src = (ROOT / "animation_system.drawio").read_text(encoding='utf-8')
GROUPS["g_tail"] = ["assets", "tbl_note", "O1", "O2"]
GROUPS["g_tbl"] = sorted(set(re.findall(r'<mxCell id="(th_[a-z]+|tr_[a-z0-9]+)"', src)))  # 武器表表头+表行
ORDER = ["g_src", "g_abp", "g_tbl", "g_tail"]   # 表在视角之前压暗/恢复
STATES = [("an_a1", ["g_src"]), ("an_a2", ["g_abp"]), ("an_a3", ["g_tail"])]
for name, cur in STATES:
    render(make_state(src, cur), name)
print("done: 3")
