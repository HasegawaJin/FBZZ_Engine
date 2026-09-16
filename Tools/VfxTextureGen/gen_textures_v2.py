"""FBZZ Engine / GreenWare — 静止テクスチャ生成 (第 2 期: 弾き・転倒・とどめ・斬撃用)。

    python gen_textures_v2.py            # 11 点を生成
    SEED=3 python gen_textures_v2.py     # 別の乱数

全て無彩色 RGB + ストレートアルファ (Textures/README.md のチャンネル規約)。
既存 13 点 (gen_textures.py) には触らない。
"""
import os
import numpy as np
from fxnoise import (rng, fbm_2d, centered_grid, uv_grid, splat_gaussians, splat_lit_spheres,
                     draw_line, blur, to_rgba, save_rgba)

SEED = int(os.environ.get("SEED", "11"))
OUT = os.environ.get("OUT", ".")
r = rng(SEED)


def smoothstep(a, b, x):
    t = np.clip((x - a) / np.maximum(b - a, 1e-6), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def write(name, lum, alpha):
    save_rgba(os.path.join(OUT, name), to_rgba(lum, alpha))
    print("wrote", name)


# ── 1. T_Slash_Arc — 斬撃の弧 (512x256) ──────────────────────────────────────
# 三日月。峰側 (上) が鋭く、刃が抜けた側 (右) ほど細く消える。ビルボードで斬った向きへ回して使う。
def slash_arc():
    W, H = 512, 256
    x, y = centered_grid(W, H)
    # 楕円弧: 中心を下へずらした円の上半分
    cxo, cyo, R = 0.0, -0.55, 1.25
    d = np.sqrt((x - cxo) ** 2 + ((y - cyo) * 1.0) ** 2)
    ang = np.arctan2(y - cyo, x - cxo)  # 0 = 右, pi = 左
    along = np.clip((np.pi - ang) / np.pi, 0.0, 1.0)  # 左端 0 → 右端 1
    # 太さは左 (振り始め) が太く、右 (抜け) で細い
    thick = 0.22 * (1.0 - 0.72 * along) + 0.025
    core = np.exp(-((d - R) / (thick * 0.35)) ** 2)
    body = np.exp(-((d - R) / thick) ** 2)
    # 上側 (y > cyo) だけ
    upper = smoothstep(-0.05, 0.10, y - cyo)
    # 両端をテーパー
    taper = smoothstep(0.0, 0.10, along) * (1.0 - smoothstep(0.86, 1.0, along))
    n = fbm_2d(*uv_grid(W, H), octaves=4, period=6, seed=SEED + 5)
    streak = 0.75 + 0.25 * np.clip((n - 0.3) / 0.4, 0, 1)
    lum = np.clip(core * 1.0 + body * 0.55, 0, 1) * upper * taper
    alpha = np.clip(body * 0.9 + core * 0.4, 0, 1) * upper * taper * streak
    write("T_Slash_Arc.png", lum, alpha)


# ── 2. T_Ring_Thin — 締まった衝撃波の輪 (512²) ─────────────────────────────
# T_Ring_Grad は柔らかい階調。こちらは «パキッ» 用の硬い輪。外縁が鋭く内側へ薄く尾を引く。
def ring_thin():
    W = H = 512
    x, y = centered_grid(W, H)
    d = np.sqrt(x * x + y * y)
    edge = np.exp(-((d - 0.88) / 0.018) ** 2)
    tail = smoothstep(0.55, 0.88, d) * (1.0 - smoothstep(0.88, 0.92, d)) * 0.35
    tail = tail * (0.3 + 0.7 * (d - 0.55) / 0.33)
    outer = np.exp(-np.clip(d - 0.88, 0, None) / 0.03) * (d > 0.88) * 0.5
    ang = np.arctan2(y, x)
    ripple = 0.85 + 0.15 * np.cos(ang * 24.0) * smoothstep(0.7, 0.88, d)
    lum = np.clip(edge + tail * 0.7 + outer, 0, 1)
    alpha = np.clip(edge + tail + outer, 0, 1) * ripple
    write("T_Ring_Thin.png", lum, alpha)


# ── 3. T_Glint_Star — 4 本の光条 (256²) ─────────────────────────────────────
def glint_star():
    W = H = 256
    x, y = centered_grid(W, H)
    d = np.sqrt(x * x + y * y)
    core = np.exp(-(d / 0.09) ** 2)
    halo = np.exp(-(d / 0.35) ** 2) * 0.35
    ray = np.zeros_like(x)
    for a, L, wdt in [(0, 0.95, 0.035), (np.pi / 2, 0.95, 0.035),
                      (np.pi / 4, 0.42, 0.02), (-np.pi / 4, 0.42, 0.02)]:
        ca, sa = np.cos(a), np.sin(a)
        u = x * ca + y * sa
        v = -x * sa + y * ca
        prof = np.exp(-(v / (wdt * (1.0 - 0.8 * np.abs(u) / L + 0.02))) ** 2)
        ray += prof * np.clip(1.0 - np.abs(u) / L, 0, 1) ** 1.6
    lum = np.clip(core + ray + halo, 0, 1)
    alpha = np.clip(core + ray * 0.9 + halo, 0, 1)
    write("T_Glint_Star.png", lum, alpha)


# ── 4. T_Shard_2x2 — 角ばった破片 4 種 (512² / 2x2) ────────────────────────────
# 装甲のかけら。面ごとに明暗を付けて «厚みのある板» に見せる。sprite_random_start_frame で使う。
def shard_atlas():
    W = H = 512
    C = 256
    lum = np.zeros((H, W), np.float32)
    alpha = np.zeros((H, W), np.float32)
    x, y = centered_grid(C, C)
    for i in range(4):
        k = 5 + (i % 3)
        ang = np.sort(r.random(k) * 2 * np.pi)
        rad = r.uniform(0.45, 0.85, k)
        px = np.cos(ang) * rad
        py = np.sin(ang) * rad
        # 凸多角形の内側判定 (半平面の積)
        inside = np.ones((C, C), bool)
        shade = np.zeros((C, C), np.float32)
        for j in range(k):
            x0, y0 = px[j], py[j]
            x1, y1 = px[(j + 1) % k], py[(j + 1) % k]
            side = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0)
            inside &= side >= 0
        # 面: 中心から遠いほど傾いた面。適当な «稜線» で 2〜3 面に割る
        nrm_a = r.uniform(0, 2 * np.pi)
        f1 = x * np.cos(nrm_a) + y * np.sin(nrm_a)
        facet = np.where(f1 > r.uniform(-0.2, 0.2), 0.50, 0.28)
        facet = facet + np.where(f1 > 0.35, 0.14, 0.0)
        # 縁を少し明るく (厚みのハイライト)
        dist_edge = np.ones((C, C), np.float32)
        for j in range(k):
            x0, y0 = px[j], py[j]
            x1, y1 = px[(j + 1) % k], py[(j + 1) % k]
            dx, dy = x1 - x0, y1 - y0
            L2 = dx * dx + dy * dy
            t = np.clip(((x - x0) * dx + (y - y0) * dy) / L2, 0, 1)
            d = np.sqrt((x - (x0 + t * dx)) ** 2 + (y - (y0 + t * dy)) ** 2)
            dist_edge = np.minimum(dist_edge, d)
        rim = smoothstep(0.06, 0.0, dist_edge) * 0.25
        grain = fbm_2d(*uv_grid(C, C), octaves=3, period=8, seed=SEED + 20 + i) * 0.18
        shade = np.clip(facet + rim + grain - 0.09, 0, 1) * inside
        a = inside.astype(np.float32)
        a = blur(a, 0.6)
        row, col = divmod(i, 2)
        lum[row * C:(row + 1) * C, col * C:(col + 1) * C] = shade
        alpha[row * C:(row + 1) * C, col * C:(col + 1) * C] = a
    write("T_Shard_2x2.png", lum, alpha)


# ── 5. T_Dust_Puff_2x2 — 土煙の単発スプライト 4 種 (512² / 2x2) ────────────────
# FlipFlop の煙は «湧いて崩れる» 1 本の芝居。こちらは軽く撒く用の静止した塊。
def dust_atlas():
    W = H = 512
    C = 256
    lum = np.zeros((H, W), np.float32)
    alpha = np.zeros((H, W), np.float32)
    for i in range(4):
        n = 26
        ang = r.random(n) * 2 * np.pi
        rad = r.random(n) ** 0.7 * 0.32 * C
        px = C * 0.5 + np.cos(ang) * rad * r.uniform(1.0, 1.3)
        py = C * 0.5 + np.sin(ang) * rad
        prad = r.uniform(0.09, 0.17, n) * C
        w = r.uniform(0.6, 1.0, n)
        dens, lit = splat_lit_spheres(C, C, px, py, prad, w, softness=0.75)
        u, v = uv_grid(C, C)
        nz = fbm_2d(u, v, octaves=4, period=5, seed=SEED + 40 + i)
        er = smoothstep(0.30, 0.62, nz)
        edge = np.clip(dens / 1.4, 0, 1)
        dens = dens * (edge + (1 - edge) * er)
        a = 1.0 - np.exp(-dens * 1.1)
        a = smoothstep(0.03, 0.4, a) * a ** 0.6
        l = 0.30 + 0.62 * np.clip(lit * 1.2 - 0.1, 0, 1) ** 1.3
        row, col = divmod(i, 2)
        lum[row * C:(row + 1) * C, col * C:(col + 1) * C] = l
        alpha[row * C:(row + 1) * C, col * C:(col + 1) * C] = a
    write("T_Dust_Puff_2x2.png", lum, alpha)


# ── 6. T_Crack_Radial — 転倒のクレーター (1024²) ──────────────────────────────
# T_Crack_Decal より大きく、中央の陥没 + 放射状のひび + 同心の段差。暗くする方向で使う。
def crack_radial():
    W = H = 1024
    x, y = centered_grid(W, H)
    d = np.sqrt(x * x + y * y)
    ang = np.arctan2(y, x)
    u, v = uv_grid(W, H)
    n = fbm_2d(u, v, octaves=5, period=6, seed=SEED + 60)
    n2 = fbm_2d(u, v, octaves=3, period=14, seed=SEED + 61)
    field = np.zeros((H, W), np.float32)
    # 放射のひび: 中心から外へ、途中で枝分かれ
    def crack(a0, x0, y0, length, width, depth=0):
        segs = 9
        a = a0
        px, py = x0, y0
        for s in range(segs):
            L = length / segs * r.uniform(0.7, 1.3)
            a += r.normal(0, 0.28)
            nx, ny = px + np.cos(a) * L, py + np.sin(a) * L
            wpx = width * (1.0 - s / segs * 0.75)
            draw_line(field, (px + 1) * 0.5 * W, (1 - py) * 0.5 * H,
                      (nx + 1) * 0.5 * W, (1 - ny) * 0.5 * H, wpx, 1.0)
            if depth < 2 and r.random() < 0.35 and s > 1:
                crack(a + r.choice([-1, 1]) * r.uniform(0.5, 1.0), nx, ny,
                      length * 0.45, width * 0.6, depth + 1)
            px, py = nx, ny
    for i in range(11):
        a0 = i / 11 * 2 * np.pi + r.normal(0, 0.15)
        crack(a0, np.cos(a0) * 0.06, np.sin(a0) * 0.06, r.uniform(0.55, 0.92), r.uniform(7.0, 11.0))
    cracks = np.clip(field, 0, 1)
    # 同心の段差 (陥没の縁)
    rings = np.exp(-((d - 0.22) / 0.02) ** 2) * 0.8 + np.exp(-((d - 0.40) / 0.025) ** 2) * 0.45
    rings = rings * (0.4 + 0.6 * smoothstep(0.25, 0.6, n2))
    # 中央の陥没 (面で暗く)
    bowl = np.exp(-(d / 0.30) ** 2) * (0.55 + 0.45 * n)
    fade = 1.0 - smoothstep(0.62, 0.98, d)
    alpha = np.clip(cracks * 0.95 + rings + bowl * 0.8, 0, 1) * fade
    # 輝度: ひびの底は暗い、縁はごく薄く明るい
    lum = np.clip(0.10 + 0.14 * (1 - cracks) + rings * 0.35 * (1 - cracks), 0, 1)
    write("T_Crack_Radial.png", lum, alpha)


# ── 7. T_Bolt — 稲妻 1 本 (512x128) ───────────────────────────────────────────
# 左端から右端へ。T_Arc_Noise はリボン用のノイズ、こちらは «1 発の放電» のスプライト。
def bolt():
    W, H = 512, 128
    field = np.zeros((H, W), np.float32)
    def branch(x0, y0, x1, y1, width, disp, depth):
        if depth == 0 or abs(x1 - x0) < 6:
            draw_line(field, x0, y0, x1, y1, width, 1.0)
            return
        mx, my = (x0 + x1) * 0.5, (y0 + y1) * 0.5
        my += r.normal(0, disp)
        branch(x0, y0, mx, my, width, disp * 0.55, depth - 1)
        branch(mx, my, x1, y1, width, disp * 0.55, depth - 1)
        if depth >= 3 and r.random() < 0.5:
            L = (x1 - x0) * r.uniform(0.25, 0.5)
            branch(mx, my, mx + L, my + r.normal(0, disp * 1.5), width * 0.5, disp * 0.5, depth - 2)
    branch(8, H * 0.5, W - 8, H * 0.5, 2.3, 26.0, 6)
    core = np.clip(field, 0, 1)
    glow = blur(core, 7.0) * 2.2
    lum = np.clip(core * 1.0 + glow * 0.7, 0, 1)
    alpha = np.clip(core + glow * 0.9, 0, 1)
    write("T_Bolt.png", lum, alpha)


# ── 8. T_Cut_Line — 溶断の縫い目 (512x64、X 方向シームレス) ────────────────────
# 脚がもげた切断面の «赤熱した線»。UV を X へスクロールさせる。
def cut_line():
    W, H = 512, 64
    u, v = uv_grid(W, H)
    n = fbm_2d(u, v * 0.25, octaves=4, period=8, seed=SEED + 80)
    n2 = fbm_2d(u, v, octaves=3, period=16, seed=SEED + 81)
    center = 0.5 + (n - 0.5) * 0.35
    dist = np.abs(v - center)
    width = 0.06 + 0.10 * n2
    core = np.exp(-(dist / (width * 0.35)) ** 2)
    body = np.exp(-(dist / width) ** 2)
    lum = np.clip(core + body * 0.5, 0, 1)
    alpha = np.clip(core * 0.9 + body * 0.7, 0, 1) * (0.7 + 0.3 * n2)
    write("T_Cut_Line.png", lum, alpha)


# ── 9. T_Burst_Rays — 放射の光条 (512²) ──────────────────────────────────────
# Just 弾きの «読み切った» 用。中心から不揃いな筋が放射する。回転させて使う。
def burst_rays():
    W = H = 512
    x, y = centered_grid(W, H)
    d = np.sqrt(x * x + y * y)
    ang = np.arctan2(y, x)
    ray = np.zeros_like(x)
    for i in range(56):
        a = r.random() * 2 * np.pi
        L = r.uniform(0.35, 0.98)
        wdt = r.uniform(0.006, 0.02)
        ca, sa = np.cos(a), np.sin(a)
        u = x * ca + y * sa
        v = -x * sa + y * ca
        prof = np.exp(-(v / (wdt + 0.012 * np.clip(u, 0, 1))) ** 2)
        along = smoothstep(0.04, 0.16, u) * (1.0 - smoothstep(L * 0.55, L, u))
        ray += prof * along * r.uniform(0.5, 1.0)
    ray = np.clip(ray, 0, 1)
    core = np.exp(-(d / 0.07) ** 2)
    lum = np.clip(ray + core, 0, 1)
    alpha = np.clip(ray * 0.95 + core, 0, 1) * (1.0 - smoothstep(0.9, 1.0, d))
    write("T_Burst_Rays.png", lum, alpha)


# ── 10. T_Ember_Dot — 火の粉 (64²) ────────────────────────────────────────────
def ember_dot():
    W = H = 64
    x, y = centered_grid(W, H)
    d = np.sqrt(x * x + y * y)
    core = np.exp(-(d / 0.18) ** 2)
    halo = np.exp(-(d / 0.6) ** 2) * 0.45
    lum = np.clip(core + halo, 0, 1)
    alpha = np.clip(core + halo, 0, 1)
    write("T_Ember_Dot.png", lum, alpha)


# ── 11. T_Streak_Long — 長い速度線 (512x64) ─────────────────────────────────────
# T_Spark_Streak (256x64) より細く長い。頭が X=88%、尾は左へ緩やかに消える。
def streak_long():
    W, H = 512, 64
    u, v = uv_grid(W, H)
    head = 0.88
    along = np.clip((u) / head, 0, 1)
    tail = along ** 2.2 * (u <= head) + np.exp(-((u - head) / 0.035) ** 2) * (u > head)
    wdt = 0.05 + 0.20 * along ** 1.5
    prof = np.exp(-((v - 0.5) / wdt) ** 2)
    core = np.exp(-((v - 0.5) / (wdt * 0.4)) ** 2) * along ** 3
    lum = np.clip(prof * (0.4 + 0.6 * along) + core, 0, 1)
    alpha = np.clip(prof * tail + core * 0.8, 0, 1)
    write("T_Streak_Long.png", lum, alpha)


if __name__ == "__main__":
    slash_arc()
    ring_thin()
    glint_star()
    shard_atlas()
    dust_atlas()
    crack_radial()
    bolt()
    cut_line()
    burst_rays()
    ember_dot()
    streak_long()
