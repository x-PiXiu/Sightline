# -*- coding: utf-8 -*-
"""第 2 期架构走查灯位序列产线（D6 服务端 s2~s5 + D7 客户端 c1~c4）
用法：python make_arch_walkthrough.py
语义（讲到哪亮哪，三态）：
  current = 亮红底 #ffcccc + 粗边 5（箭头粗 4）——本张口播正讲的模块
  past    = 定稿常态（#ffe6cc 橙）——已讲过的模块
  future  = opacity=35 压暗——还没讲到的模块
s1 复用定稿 server_architecture.png（口播"灰=第1期底座，橙=新长"正是定稿语义）。
旧层元素定稿自带 opacity=30 永不改动；整页导出不裁剪，同组同几何，剪映硬切不跳帧。

坑沿用 ep01/ue_steps 产线（2026-10-08 实测）：
  draw.io CLI 快速连渲会拿陈旧产物——唯一临时名渲染 + mtime 新鲜断言 + 重试；
  压暗断言采全 bbox 会被透底污染（2026-10-09 教训）——本产线像素机检采边框/角落条带。
"""
import re, shutil, subprocess, sys, time, pathlib
from PIL import Image

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep02")
OUT = ROOT / "arch_walk"
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"
SCALE = 2

# ---- 服务端：新层（橙红）元素全量；旧层定稿已 opacity=30 不动 ----
# flu（调 用例箭头）不参与三态：其竖直下降段横穿 bandapp 层标题文字，
# 加粗态压字（visual-judge 2026-10-11 判 s3/s4/s5 fail 的根因）——恒定定稿原样
SV_ALL = ["b_disp", "b_writer", "b_lobby", "b_room", "b_player",
          "f5n", "fde", "lop", "lwr", "lev", "levlbl"]
SV_STATES = {  # 名 → (current ids, past ids)；其余新层 = future
    "s2": (["b_disp", "f5n", "fde"], []),
    "s3": (["b_lobby"], ["b_disp", "f5n", "fde"]),
    "s4": (["b_room", "b_player", "lop"], ["b_disp", "f5n", "fde", "b_lobby"]),
    "s5": (["b_writer", "lwr", "lev", "levlbl"],
           ["b_disp", "f5n", "fde", "b_lobby", "b_room", "b_player", "lop"]),
}
SV_SRC = ROOT / "server_architecture.drawio"

# ---- 客户端：新层元素全量 ----
CL_ALL = ["b_sess", "b_model", "b_ui", "nf1", "nf2", "nf3", "nf4", "nf5"]
CL_STATES = {
    "c1": (["b_ui"], []),
    "c2": (["b_ui", "nf4"], []),
    "c3": (["b_sess", "nf1", "nf2"], ["b_ui", "nf4"]),
    "c4": (["b_model", "nf3", "nf5"], ["b_ui", "nf4", "b_sess", "nf1", "nf2"]),
}
CL_SRC = ROOT / "client_architecture.drawio"

# 几何（像素机检用）：id → (x, y, w, h)——与定稿源一致，改样式不改几何
SV_BOX = {"b_disp": (700, 308, 190, 64), "b_writer": (910, 308, 190, 64),
          "b_lobby": (80, 636, 320, 64), "b_room": (460, 636, 260, 64),
          "b_player": (780, 636, 240, 64)}
CL_BOX = {"b_sess": (70, 360, 320, 76), "b_model": (450, 360, 300, 76),
          "b_ui": (830, 360, 330, 76)}


def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*style=")([^"]*)(")' % re.escape(sid))
    m = pat.search(xml)
    assert m, f"id 不存在: {sid}"
    return pat.sub(lambda mm: mm.group(1) + fn(mm.group(2)) + mm.group(3), xml, count=1)


def cur_fn(st):
    if "edgeLabel" in st:                      # 边标签无底色，保持原样（红字已醒目）
        return st
    st = st.replace("fillColor=#ffe6cc", "fillColor=#ffcccc")
    st = re.sub(r";strokeWidth=[\d.]+", "", st)
    return st + ";strokeWidth=5"


def dim_fn(st):
    return st + ";opacity=35"


def make_state(src_xml, all_ids, cur, past):
    xml = src_xml
    for sid in all_ids:
        if sid in cur:
            xml = sub_style(xml, sid, cur_fn)
        elif sid not in past:
            xml = sub_style(xml, sid, dim_fn)
    return xml


def check_styles(xml, all_ids, cur, past, name):
    """生成侧机检：current 有粗边/亮底，future 有压暗，past 干净。"""
    for sid in all_ids:
        m = re.search(r'<mxCell id="%s" [^>]*style="([^"]*)"' % re.escape(sid), xml)
        assert m, f"{name}: id 丢失 {sid}"
        st = m.group(1)
        if sid in cur:
            if "edgeLabel" not in st:
                assert "strokeWidth=5" in st, f"{name}: {sid} current 缺粗边"
                if "fillColor=" in st:
                    assert "#ffcccc" in st, f"{name}: {sid} current 缺亮底"
        elif sid not in past:
            assert "opacity=35" in st, f"{name}: {sid} future 未压暗"
        else:
            assert "opacity=35" not in st and "#ffcccc" not in st, f"{name}: {sid} past 被污染"


def render_fresh(drawio, png, tries=3):
    png = pathlib.Path(png)
    for k in range(tries):
        tmp = png.with_name(f"{png.stem}.fresh{time.time_ns()}.png")
        r = subprocess.run([DRAWIO, "-x", "-f", "png", "-s", str(SCALE),
                            "-o", str(tmp), str(drawio)],
                           capture_output=True, text=True, timeout=120)
        time.sleep(0.4)
        if r.returncode == 0 and tmp.exists() and tmp.stat().st_mtime > pathlib.Path(drawio).stat().st_mtime:
            shutil.move(tmp, png)
            return
        print(f"  ⚠️ {png.name} 第{k+1}次渲染陈旧/失败，重试")
        time.sleep(1.0)
    raise RuntimeError(f"渲染反复陈旧: {png}")


def color_count_check(png, cur, past, vertex_ids):
    """像素机检（几何无关版）：drawio CLI -s 2 按内容 bbox 出图（非页面×2），
    坐标采样必错位（2026-10-11 实测 2302x1562≠2400x1640）——改特征色计数：
    亮红填充 #ffcccc = current 框专属；饱和橙 #ffe6cc = past 框专属
    （future opacity=35 渲染后 ≈(255,246,237)，tol=3 不落入判据）。"""
    im = Image.open(png).convert("RGB")
    px = im.getdata()
    def count(color, tol=3):
        r0, g0, b0 = color
        return sum(1 for (r, g, b) in px
                   if abs(r - r0) <= tol and abs(g - g0) <= tol and abs(b - b0) <= tol)
    red = count((255, 204, 204))
    orange = count((255, 230, 204))
    n_cur_v = len([i for i in cur if i in vertex_ids])
    n_past_v = len([i for i in past if i in vertex_ids])
    assert red >= 1500 * max(n_cur_v, 1), f"{png.name}: 亮红填充不足 red={red}（current 未生效？）"
    if n_past_v == 0:
        assert orange < 3000, f"{png.name}: past 为空但饱和橙={orange}（future 未压暗？）"
    else:
        assert orange >= 2000 * n_past_v, f"{png.name}: past 饱和橙不足 orange={orange}"


def run_group(tag, src, all_ids, states, box_map):
    src_xml = src.read_text(encoding="utf-8")
    for name, (cur, past) in states.items():
        xml = make_state(src_xml, all_ids, set(cur), set(past))
        check_styles(xml, all_ids, set(cur), set(past), name)
        d = OUT / f"{name}.drawio"
        d.write_text(xml, encoding="utf-8")
        p = OUT / f"{name}.png"
        render_fresh(d, p)
        color_count_check(p, set(cur), set(past), [i for i in box_map])
        print(f"✓ {name}  cur={cur}")
    # 同组两两不同（md5）
    import hashlib
    digs = {}
    for name in states:
        h = hashlib.md5((OUT / f"{name}.png").read_bytes()).hexdigest()
        assert h not in digs.values(), f"{tag}: {name} 与 {digs} 内容重复"
        digs[name] = h


def main():
    OUT.mkdir(exist_ok=True)
    run_group("server", SV_SRC, SV_ALL, SV_STATES, SV_BOX)
    run_group("client", CL_SRC, CL_ALL, CL_STATES, CL_BOX)
    # s1 = 定稿全图原样（走查起点）
    shutil.copyfile(ROOT / "server_architecture.png", OUT / "s1.png")
    files = sorted(p.name for p in OUT.glob("*.png"))
    print("✓ s1.png（复用定稿）")
    print("产物：" + " ".join(files))
    sizes = {p.name: Image.open(p).size for p in OUT.glob("*.png")}
    print("几何：" + str(sizes))


if __name__ == "__main__":
    sys.exit(main())
