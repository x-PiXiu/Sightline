# -*- coding: utf-8 -*-
"""第 3 期客户端架构走查产线 c1~c7（S4 执行顺序：四器官→输入→上报→收包→入账→变画面→供数）
三态语义同 arch_walk（文字焦点红粗 / 框剥暗化 / future 压暗35%）。
坑沿用：唯一临时名+mtime 重试。
"""
import re, subprocess, time, pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep03")
OUT = ROOT / "client_walk"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"

ORDER = ["g_match", "g_reg", "g_char", "e_m2r", "e_m2r_lbl", "g_input", "e_i2c", "e_i2c_lbl",
         "e_mrep", "e_badge", "e_badge_lbl", "b2", "b3", "b4", "g_slider", "e_s2m", "e_s2m_lbl",
         "e_b4s", "e_r2c", "e_r2c_lbl", "e_c2a", "e_c2a_lbl", "g_abp"]
GROUPS = {k: [k] for k in ORDER}          # 一元素一组，组序即执行序
STATES = [   # (名, current 元素)；past/future 由 ORDER 索引推导
    ("c1", ["g_match", "g_reg", "g_char", "e_m2r", "e_m2r_lbl"]),        # 四器官齐亮
    ("c2", ["g_input", "e_i2c", "e_i2c_lbl"]),                            # 输入
    ("c3", ["g_char", "e_mrep", "g_match", "e_badge", "e_badge_lbl"]),    # 采样上报
    ("c4", ["b2", "b3", "b4", "g_slider", "e_s2m", "e_s2m_lbl"]),         # 收包（滑块扣延迟处）
    ("c5", ["g_match", "e_b4s", "g_reg"]),                                # 解析入账
    ("c6", ["g_reg", "e_m2r", "e_m2r_lbl", "g_char", "e_r2c", "e_r2c_lbl"]),  # 变画面（省略④）
    ("c7", ["g_char", "e_c2a", "e_c2a_lbl", "g_abp"]),                    # 供动画数
]


def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*?style=")([^"]*)(")' % re.escape(sid))
    assert pat.search(xml), f"id 不存在: {sid}"
    return pat.sub(lambda m: m.group(1) + fn(m.group(2)) + m.group(3), xml, count=1)


def make_state(src, cur_ids):
    idx = {k: i for i, k in enumerate(ORDER)}
    cur_ix = max(idx[c] for c in cur_ids)
    xml = src
    for sid in cur_ids:                                            # current 亮
        def cur(st):
            if 'endArrow=' in st:
                return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=6', st) if 'strokeWidth=' in st else st + ';strokeWidth=6'
            if 'rounded=' in st and 'fillColor=' in st:
                st = re.sub(r'fillColor=#[0-9a-fA-F]+', 'fillColor=#ffcccc', st)
                st = re.sub(r'strokeColor=#[0-9a-fA-F]+', 'strokeColor=#c8161d', st)
                st = re.sub(r';?opacity=\d+', '', st)
                return re.sub(r'strokeWidth=[\d.]+', 'strokeWidth=5', st) if 'strokeWidth=' in st else st + ';strokeWidth=5'
            if 'text;' in st:
                st = re.sub(r'fontColor=#[0-9a-fA-F]+', 'fontColor=#c8161d', st)
                return st + ';fontStyle=1'
            return st
        xml = sub_style(xml, sid, cur)
    for sid in ORDER[cur_ix + 1:]:                                 # future 压暗
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


src = (ROOT / "client_architecture.drawio").read_text(encoding='utf-8')
for name, cur in STATES:
    render(make_state(src, cur), name)
print("done: 7")
