# -*- coding: utf-8 -*-
"""第 3 期正片三图走查灯位序列产线（分镜 4/6/7/10 的"讲到哪亮哪"变体）
语义沿用 ep02 make_arch_walkthrough（已验证）：
  current = 亮红底 #ffcccc + 粗边（框 5 / 箭头 4.5）——口播正讲的组
  past    = 定稿常态——已讲过
  future  = opacity=35 压暗——还没讲到
  文字类不加红底（焦点靠"其余压暗"衬托）；黄虚线演进/标题/图例/生命线恒定。
坑沿用：唯一临时名渲染 + mtime 新鲜断言 + 重试（drawio CLI 连渲拿陈旧产物）。
"""
import re, subprocess, time, pathlib, shutil

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep03")
OUT = ROOT / "walk"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"
SCALE = "2"

# ---- match_dataflow：d1~d6（分镜4 派活逐条 + 分镜6 验收结论聚焦）----
MD_ALL = {
    "g14":   ["m14", "m14lbl", "m14lbl2"],
    "g15":   ["m15a", "m15albl", "m15b", "m15blbl", "mAtravel", "mAtravellbl", "mBtravel", "mBtravellbl"],
    "g16":   ["m16", "m16lbl", "m16lbl2", "m16red", "m16redline"],
    "gtick": ["mtick", "mticklbl"],
    "g17":   ["m17", "m17lbl"],
    "g18":   ["m18a", "m18albl", "m18b", "m18blbl"],
    "gconc": ["conclusion"],
}
# 注：直设组（gset/msetred 省略④）不入三态——常亮定稿（省略④归 S4 客户端段指认）
MD_STATES = [
    ("md_d1", ["g14"]),
    ("md_d2", ["g15"]),
    ("md_d3", ["g16"]),
    ("md_d4", ["gtick", "g17"]),
    ("md_d5", ["g18"]),
    ("md_d6", ["gconc"]),
]

# ---- input_pipeline：i1~i8（分镜7 逐跳 + 关键句聚焦）----
IP_ALL = {
    "p1": ["n1"],
    "p2": ["n2"],
    "p3": ["n3"],
    "p4": ["n4", "pit", "pitline"],
    "p5": ["n5"],
    "p6": ["n6"],
    "p7": ["n7"],
    "pkey": ["keyline"],
}
IP_STATES = [
    ("ip_i1", ["p1"]), ("ip_i2", ["p2"]), ("ip_i3", ["p3"]), ("ip_i4", ["p4"]),
    ("ip_i5", ["p5"]), ("ip_i6", ["p6"]), ("ip_i7", ["p7"]), ("ip_i8", ["pkey"]),
]

# ---- latency_composition：l1~l5（分镜10 四段扫过 + 关键句聚焦）----
LC_ALL = {
    "gsrc":  ["src", "arrow0"],
    "gseg1": ["seg1", "seg1name", "seg1sub", "seg1lvl", "seg1fix", "arrow1"],
    "gseg2": ["seg2", "seg2name", "seg2sub", "seg2lvl", "seg2fix", "arrow2"],
    "gseg3": ["seg3", "seg3name", "seg3sub", "seg3lvl", "seg3fix", "arrow3"],
    "gseg4": ["seg4", "seg4name", "seg4sub", "seg4lvl", "seg4fix", "arrow4", "dst"],
    "gkey":  ["keyline"],
}
LC_STATES = [
    ("lc_l1", ["gseg1"]),
    ("lc_l2", ["gseg2"]),
    ("lc_l3", ["gseg3"]),
    ("lc_l4", ["gseg4"]),
    ("lc_l5", ["gkey"]),
]


def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*?style=")([^"]*)(")' % re.escape(sid))
    m = pat.search(xml)
    assert m, f"id 不存在: {sid}"
    return pat.sub(lambda mm: mm.group(1) + fn(mm.group(2)) + mm.group(3), xml, count=1)


def make_state(src_xml, all_groups, cur_groups):
    """cur_groups=本张 current 的组名；past=已讲过的组（常态）；future=其余组压暗。
    语义：组序列里 current 之前的全 past，之后的全 future。"""
    names = list(all_groups.keys())
    cur_ix = max(names.index(g) for g in cur_groups)
    past, future = names[:cur_ix], names[cur_ix + 1:]
    xml = src_xml
    for g in cur_groups:
        for sid in all_groups[g]:
            def cur(st):
                if 'endArrow=' in st:                       # 箭头
                    if 'strokeWidth=' in st:
                        st = re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=6', st)
                    else:
                        st += ';strokeWidth=6'
                    return st
                if 'rounded=' in st and 'fillColor=' in st:  # 框
                    st = re.sub(r'fillColor=#[0-9a-fA-F]+', 'fillColor=#ffcccc', st)
                    if 'strokeWidth=' in st:
                        st = re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=5', st)
                    else:
                        st += ';strokeWidth=5'
                    return st
                return st                                    # 文字/虚线：保持
            xml = sub_style(xml, sid, cur)
    for g in future:
        for sid in all_groups[g]:
            # drawio 坑：纯文字 cell 的字体透明度归 textOpacity 管，只加 opacity 文字不淡（lc_l1 实测）
            xml = sub_style(xml, sid, lambda st: st + ';opacity=35;textOpacity=35')
    return xml


def render(xml, name):
    tmp = ROOT / f"_tmp_{name}.drawio"
    png = OUT / f"{name}.png"
    tmp.write_text(xml, encoding='utf-8')
    for _ in range(3):
        before = png.stat().st_mtime if png.exists() else 0
        subprocess.run([DRAWIO, "-x", "-f", "png", "-s", SCALE,
                        "-o", str(png), str(tmp)], capture_output=True)
        if png.exists() and png.stat().st_mtime > before:
            break
        time.sleep(1.2)
    else:
        raise RuntimeError(f"渲染失败: {name}")
    tmp.unlink()
    print(f"  {png.name}  {png.stat().st_size//1024}KB")


for src_name, all_groups, states in [
    ("match_dataflow", MD_ALL, MD_STATES),
    ("input_pipeline", IP_ALL, IP_STATES),
    ("latency_composition", LC_ALL, LC_STATES),
]:
    src = (ROOT / f"{src_name}.drawio").read_text(encoding='utf-8')
    print(f"{src_name}:")
    for name, cur in states:
        render(make_state(src, all_groups, cur), name)
print("done:", len(list(OUT.glob('*.png'))), "variants")
