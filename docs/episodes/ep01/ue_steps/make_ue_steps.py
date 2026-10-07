# -*- coding: utf-8 -*-
"""UE 客户端灯位图卡产线（S5-02~07，服务端 steps/ 同款语法）
用法：python make_ue_steps.py   （生成 c0~c5 .drawio，逐张渲 PNG，渲后自动新鲜度校验）
变体表：c0 全图素颜 / c1 b1 GameInstance / c2 b2 接收线程 / c3 b6 心跳计时器 /
        c4 b5 HUD / c5 全图恢复+rule 铁律条红字
约定（与服务端 step_0~7 一致）：高亮=浅橙底#ffe6cc+主红粗边4；压暗框 opacity=30、
箭头 opacity=25；标题/容器带始终全亮；整页导出不裁剪（六张同几何，剪映硬切不跳帧）。

坑（2026-10-08 实测）：draw.io CLI 快速连渲多张时会拿到陈旧产物（实例复用竞态，
同输出路径前一张内容残留）——对策：唯一临时名渲染+mtime 新鲜度断言+重试。
"""
import re, shutil, subprocess, sys, time, pathlib

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep01")
SRC = ROOT / "ue_connection.drawio"
OUT = ROOT / "ue_steps"
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"

BOXES = ["b1", "b2", "b3", "b4", "b5", "b6"]
EDGES = ["f1", "f2", "f3", "f4", "f5"]
SMALL = ["sm1", "sm2", "sm3", "sm4", "smnote", "rule"]

HL_FILL, HL_STROKE = "#ffe6cc", "#c8161d"


def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*style=")([^"]*)(")' % re.escape(sid))
    m = pat.search(xml)
    assert m, f"id 不存在: {sid}"
    return pat.sub(lambda mm: mm.group(1) + fn(mm.group(2)) + mm.group(3), xml, count=1)


def make(hl=None, rule_red=False, dim_others=True, name=""):
    xml = SRC.read_text(encoding="utf-8")

    def hl_box(st):
        st = st.replace("fillColor=#ffffff", f"fillColor={HL_FILL}")
        if f"strokeColor={HL_STROKE}" not in st:
            st = re.sub(r"strokeColor=#[0-9a-f]{6}", f"strokeColor={HL_STROKE}", st)
        return st + ";strokeWidth=4"

    for sid in BOXES + EDGES + SMALL:
        if sid == hl:
            xml = sub_style(xml, sid, hl_box)
        elif sid == "rule" and rule_red:
            xml = sub_style(xml, sid, lambda st: st.replace("fontColor=#555555", "fontColor=#c8161d;fontStyle=1"))
        elif dim_others:
            dim = ";opacity=25" if sid in EDGES else ";opacity=30"
            xml = sub_style(xml, sid, lambda st, d=dim: st + d)
    p = OUT / f"{name}.drawio"
    p.write_text(xml, encoding="utf-8")
    return p


def render_fresh(drawio, png, tries=3):
    """唯一临时名（时间戳）渲染 → mtime 断言新鲜 → 原子改名。
    实测：draw.io CLI 对同一输出路径可能复用旧渲染内容；批产后的像素机检是最终裁判。"""
    png = pathlib.Path(png)
    for k in range(tries):
        tmp = png.with_name(f"{png.stem}.fresh{time.time_ns()}.png")
        r = subprocess.run([DRAWIO, "-x", "-f", "png", "-s", "2",
                            "-o", str(tmp), str(drawio)],
                           capture_output=True, text=True, timeout=120)
        time.sleep(0.4)                      # Electron 落盘缓冲
        if r.returncode == 0 and tmp.exists() and tmp.stat().st_mtime > pathlib.Path(drawio).stat().st_mtime:
            shutil.move(tmp, png)
            return
        print(f"  ⚠️ {png.name} 第{k+1}次渲染陈旧/失败，重试")
        time.sleep(1.0)
    raise RuntimeError(f"渲染反复陈旧: {png}")


def main():
    OUT.mkdir(exist_ok=True)
    jobs = [
        ("c0", dict()),                                        # 素颜全图
        ("c1", dict(hl="b1")),
        ("c2", dict(hl="b2")),
        ("c3", dict(hl="b6")),
        ("c4", dict(hl="b5")),
        ("c5", dict(rule_red=True, dim_others=False)),         # 全图恢复+铁律红字
    ]
    for name, kw in jobs:
        d = make(name=name, **kw)
        png = OUT / f"{name}.png"
        if name == "c0":                                       # 素颜=定稿原图原样
            shutil.copyfile(SRC, d)
        render_fresh(d, png)
        print(f"✓ {name}  {kw or '素颜'}")
    print("六张灯位卡生成完毕（同几何 2246x994，硬切安全）")


if __name__ == "__main__":
    sys.exit(main())
