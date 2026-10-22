# -*- coding: utf-8 -*-
"""第 4 期正片走查灯位序列产线（周报脚本 S3-A / S3-C 的"讲到哪亮哪"变体）
工艺沿用 ep03 make_ep03_walkthrough（已验证）：
  current = 亮红底 #ffcccc + 粗边（框 5 / 箭头 6）——口播正讲的组
  past    = 定稿常态——已讲过
  future  = opacity=35 压暗——还没讲到
  文字类不加红底（焦点靠"其余压暗"衬托）；标题/图例/生命线/演进虚线恒定。
两套底图：
  reconcile_sequence（和解时序定稿 v1）→ w1_1~w1_8：S3-A 上半场 4 拍 + S4-B 下半场 4 拍
    W1-0 = 原图复用（walk/w1_0.png = 底图拷贝）
  net_reactor（主从 Reactor）→ w2_1~w2_5：S3-C Reactor 详讲 5 拍
    W2-0 = 原图复用（walk/w2_0.png = 底图拷贝）
坑沿用 ep03：唯一临时名渲染 + mtime 新鲜断言 + 重试（drawio CLI 连渲拿陈旧产物）。
"""
import re, subprocess, time, pathlib, shutil

ROOT = pathlib.Path(r"E:\workspace\Demo\Sightline\docs\episodes\ep04")
OUT = ROOT / "walk"
OUT.mkdir(exist_ok=True)
DRAWIO = r"D:\DevSoft\Drawio\draw.io\draw.io.exe"
SCALE = "2"

# ---- reconcile_sequence：w1_1~w1_8（S3-A 走上半场 / S4-B 接下半场，同图脊柱）----
RS_ALL = {
    "g_up":    ["cliBox1", "msg1", "msg1lab"],                          # 19 号上行
    "g_auth":  ["srvBox", "srvCheatConn", "srvCheat"],                  # 权威推算+钳制（含红虚线非法路径）
    "g_snap":  ["msg2", "msg2lab"],                                     # 17 v2 下发
    "g_ret":   ["retEdge", "retLab"],                                   # 回环暗示（上半场收口）
    "g_recon": ["cliBox2", "gateConn", "gate"],                         # 三方对账
    "g_pass":  ["edgeLt", "ltLab", "boxPass"],                          # <ε 绿分支
    "g_redo":  ["edgeGe", "geLab", "boxRedo"],                          # ≥ε 红分支
    "g_gap":   ["edgeGap", "gapLab", "boxGap"],                         # 断档兜底（虚线）
}
RS_STATES = [
    ("w1_1", ["g_up"]),      # S3-02 上行改了
    ("w1_2", ["g_auth"]),    # S3-03 服务器接手+钳制
    ("w1_3", ["g_snap"]),    # S3-04 下发三件套
    ("w1_4", ["g_ret"]),     # S3-05 回环暗示 → 转场客户端
    ("w1_5", ["g_recon"]),   # S4-04 三方对账
    ("w1_6", ["g_pass"]),    # S4-05 <ε 无感通过
    ("w1_7", ["g_redo"]),    # S4-06 ≥ε 回滚重放（老师批改）
    ("w1_8", ["g_gap"]),     # S4-07 断档兜底
]

# ---- net_reactor：w2_1~w2_5（S3-C 主从 Reactor 详讲）----
NR_ALL = {
    "g_main":   ["bandmain", "bandmain_l", "b_acc", "b_tick", "b_biz", "b_fw"],  # main 只留四样
    "g_hand":   ["e_hand"],                                                        # queueInLoop 交接
    "g_worker": ["bandw", "bandw_l", "w1", "w2", "wnote"],                         # IO Worker 收字节
    "g_queue":  ["bq", "e_push", "e_drain"],                                       # frame_queue 唯一门
    "g_send":   ["e_send"],                                                        # 回程 outbound+EPOLLOUT
    "g_concl":  ["concl"],                                                         # 验收条
}
NR_STATES = [
    ("w2_1", ["g_main"]),                    # S3-07 分家：main 留四样
    ("w2_2", ["g_hand", "g_worker"]),        # S3-08 worker 收字节/切帧/投队列
    ("w2_3", ["g_queue"]),                   # S3-09 唯一的门（呼应 ep01"门就这一扇"）
    ("w2_4", ["g_send"]),                    # S3-10 回程 outbound + 业务层零改动收口
    ("w2_5", ["g_concl"]),                   # S3-14 验收+压测数据
]


def sub_style(xml, sid, fn):
    pat = re.compile(r'(<mxCell id="%s" [^>]*?style=")([^"]*)(")' % re.escape(sid))
    m = pat.search(xml)
    assert m, f"id 不存在: {sid}"
    return pat.sub(lambda mm: mm.group(1) + fn(mm.group(2)) + mm.group(3), xml, count=1)


def make_state(src_xml, all_groups, cur_groups):
    """cur_groups=本张 current 的组名；past=组序列里 current 之前的（常态）；future=之后的（压暗）。"""
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
            # drawio 坑：纯文字 cell 的字体透明度归 textOpacity 管，只加 opacity 文字不淡（ep03 实测）
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
    ("reconcile_sequence", RS_ALL, RS_STATES),
    ("net_reactor", NR_ALL, NR_STATES),
]:
    src = (ROOT / f"{src_name}.drawio").read_text(encoding='utf-8')
    print(f"{src_name}:")
    # W1-0 / W2-0 = 底图原样拷贝（全图入场帧，零渲染）
    base_png = ROOT / f"{src_name}.png"
    shutil.copyfile(base_png, OUT / f"w{1 if src_name=='reconcile_sequence' else 2}_0.png")
    for name, cur in states:
        render(make_state(src, all_groups, cur), name)
print("done:", len(list(OUT.glob('*.png'))), "frames")
