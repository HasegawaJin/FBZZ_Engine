"""FBZZ Engine / GreenWare — 爆発 / 煙 FlipFlop 生成 (v3)。

    MODE=explosion python gen_flip.py
    MODE=smoke     python gen_flip.py
    SEED=12 / RES=256 (1コマ 128px) / OUT=path も環境変数で指定できる。

v2 からの変更 (README「残っている弱点」への対応):
  ① 炎のフィラメント — 中心から放たれる筋状の火 (curl ノイズで曲がる) を序盤 22 コマに描く
  ② 房ごとにバラバラに消える — ローブごとに消散の時刻オフセットを持たせ、一様な減衰を止めた
  ③ 立ち上がりを速く — 1 コマ目で既に最終径の 25%、6 コマ目で 70%。命中感は立ち上がりで決まる
  ④ ノイズ侵食を膨張に追従させる — ノイズ座標を径で割り、模様が塊と一緒に外へ流れる
出力は無彩色 RGB + ストレートアルファ (Textures/README.md のチャンネル規約)。
"""
import os
import numpy as np
from fxnoise import (rng, fbm_3d, splat_gaussians, splat_lit_spheres, draw_line, self_shadow, blur,
                     to_rgba, save_rgba, preview_over_black)

MODE = os.environ.get("MODE", "explosion")
SEED = int(os.environ.get("SEED", "7"))
RES = int(os.environ.get("RES", "2048"))
OUT = os.environ.get("OUT", f"FX_{'Explosion' if MODE == 'explosion' else 'Smoke'}_8x8.png")
COLS = ROWS = 8
FRAMES = COLS * ROWS
CELL = RES // COLS

if MODE == "explosion":
    P = dict(
        RMAX=0.36,       # 最終半径 (コマ幅に対する比)
        TAU=0.11,        # 膨張の時定数。小さいほど速く立ち上がる
        LOBES=7,
        PUFFS=34,
        CORE=30,
        ABS=1.15,        # アルファの吸収係数。上げすぎると板になる
        HEAT_T=0.26,     # 白熱が冷める時定数
        FILAMENTS=40,
        FIL_T=0.10,      # フィラメントの寿命 (時定数)
        FIL_SPEED=1.45,  # フィラメントの先端速度 (RMAX 比)
        RISE=0.05,
        DISS0=0.42,      # 消散が始まる時刻 (ローブ平均)
        DISS_SPREAD=0.30,  # ローブごとの消散時刻のばらつき
        DISS_W=0.22,     # 消散の幅
        NOISE_TH=(0.22, 0.62),  # 侵食しきい値 (開始, 終了)
        SMOKE_L=(0.20, 0.88),   # 煙部分の輝度 (影, 光)
        SHADOW=0.16,
    )
else:
    P = dict(
        RMAX=0.31, TAU=0.22, LOBES=6, PUFFS=34, CORE=30, ABS=0.9, HEAT_T=0.0,
        FILAMENTS=0, FIL_T=1.0, FIL_SPEED=1.0, RISE=0.22,
        DISS0=0.50, DISS_SPREAD=0.28, DISS_W=0.30,
        NOISE_TH=(0.20, 0.58), SMOKE_L=(0.18, 0.92), SHADOW=0.14,
    )

r = rng(SEED)


def smoothstep(a, b, x):
    t = np.clip((x - a) / np.maximum(b - a, 1e-6), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def radius_at(t):
    return P["RMAX"] * (1.0 - np.exp(-(t + 0.035) / P["TAU"]))


# ── 房 (ローブ) と粒の配置を先に決めて、全コマで同じ粒を追う ─────────────────
lobe_angle = r.random(P["LOBES"]) * 2 * np.pi + np.arange(P["LOBES"]) * 0  # ランダム角
lobe_angle = np.sort(lobe_angle)
lobe_mag = r.uniform(0.62, 1.10, P["LOBES"])
lobe_diss = P["DISS0"] + (r.random(P["LOBES"]) - 0.5) * 2 * P["DISS_SPREAD"]

puff_lobe, puff_s, puff_tan, puff_jit, puff_seed = [], [], [], [], []
for L in range(P["LOBES"]):
    n = P["PUFFS"]
    puff_lobe += [L] * n
    puff_s += list(r.uniform(0.10, 1.0, n) ** 0.7)
    puff_tan += list(r.normal(0, 0.22, n))
    puff_jit += list(r.normal(0, 0.05, (n, 2)))
    puff_seed += list(r.random(n))
puff_lobe = np.array(puff_lobe)
puff_s = np.array(puff_s)
puff_tan = np.array(puff_tan)
puff_jit = np.array(puff_jit)
puff_seed = np.array(puff_seed)
# 描画順 = 奥から手前。房の根元を先に、外側の粒を後に描くと外の粒が手前に出る
order = np.argsort(puff_s + puff_seed * 0.3)
puff_lobe, puff_s, puff_tan, puff_jit, puff_seed = (
    puff_lobe[order], puff_s[order], puff_tan[order], puff_jit[order], puff_seed[order])

core_dir = r.random(P["CORE"]) * 2 * np.pi
core_rad = r.random(P["CORE"]) ** 0.6 * 0.45
core_seed = r.random(P["CORE"])
core_diss = P["DISS0"] + 0.15 + r.random(P["CORE"]) * 0.2   # 芯は最後まで残る

fil_dir = r.random(P["FILAMENTS"]) * 2 * np.pi
fil_speed = r.uniform(0.75, 1.0, P["FILAMENTS"]) * P["FIL_SPEED"]
fil_curl = r.normal(0, 1.0, P["FILAMENTS"])
fil_life = r.uniform(0.7, 1.3, P["FILAMENTS"])
fil_w = r.uniform(1.0, 2.2, P["FILAMENTS"])

# 侵食ノイズ用の格子 (コマ単位)
ys, xs = np.mgrid[0:CELL, 0:CELL].astype(np.float32)
cx = CELL * 0.5
cy = CELL * (0.5 + P["RISE"] * 0.45)   # 上昇ぶんを見越して初期位置を下げる
nx0 = (xs - cx) / CELL
ny0 = (ys - cy) / CELL


def fil_pos(t):
    """フィラメント先端の位置 (コマ内の [-0.5,0.5] 座標)。"""
    reach = P["RMAX"] * (1.0 - np.exp(-(t + 0.02) / P["FIL_T"] * 0.9)) * fil_speed
    ang = fil_dir + fil_curl * 0.9 * reach / max(P["RMAX"], 1e-6) * t
    return np.cos(ang) * reach, -np.sin(ang) * reach - t * P["RISE"] * 0.5


atlas = np.zeros((RES, RES, 4), dtype=np.uint8)

for f in range(FRAMES):
    t = (f + 0.5) / FRAMES
    R = radius_at(t)
    Rpx = R * CELL
    rise = P["RISE"] * t * CELL

    # ── 粒の位置・半径・重み ───────────────────────────────────────────────
    ang = lobe_angle[puff_lobe] + puff_tan * (0.9 - 0.4 * puff_s)
    dist = (0.18 + 0.82 * puff_s) * lobe_mag[puff_lobe] * Rpx
    px = cx + np.cos(ang) * dist + puff_jit[:, 0] * Rpx
    py = cy - np.sin(ang) * dist + puff_jit[:, 1] * Rpx - rise * (0.6 + 0.4 * puff_s)
    prad = (0.085 + 0.10 * puff_s) * Rpx * (0.75 + 0.6 * t) + 1.2
    # 房ごとの消散 + 粒ごとの微妙なずれ
    diss_t = lobe_diss[puff_lobe] + (puff_seed - 0.5) * 0.12
    alive = 1.0 - smoothstep(diss_t, diss_t + P["DISS_W"], t)
    pw = alive * (0.9 + 0.3 * puff_seed)

    cpx = cx + np.cos(core_dir) * core_rad * Rpx
    cpy = cy - np.sin(core_dir) * core_rad * Rpx - rise * 0.5
    crad = (0.12 + 0.10 * core_seed) * Rpx * (0.7 + 0.6 * t) + 1.2
    calive = 1.0 - smoothstep(core_diss, core_diss + P["DISS_W"] * 1.3, t)
    cw = calive * 1.1

    density, lit_sum = splat_lit_spheres(
        CELL, CELL, np.concatenate([cpx, px]), np.concatenate([cpy, py]),
        np.concatenate([crad, prad]), np.concatenate([cw, pw]), softness=0.7)
    sphere_lit = lit_sum

    # ── 膨張に追従する 3 段ノイズ侵食 ────────────────────────────────────────
    scale = max(R, 0.06)
    nxx = (nx0 / scale) * 0.55 + 0.5
    nyy = (ny0 / scale) * 0.55 + 0.5
    zt = np.full_like(nxx, t * 1.6)
    n_low = fbm_3d(nxx, nyy, zt, octaves=2, period=2, zperiod=2, seed=SEED * 3 + 1)
    n_mid = fbm_3d(nxx * 2.3, nyy * 2.3, zt * 1.3, octaves=2, period=4, zperiod=3, seed=SEED * 3 + 2)
    n_hi = fbm_3d(nxx * 6.0, nyy * 6.0, zt * 1.6, octaves=2, period=8, zperiod=4, seed=SEED * 3 + 3)
    noise = 0.5 * n_low + 0.33 * n_mid + 0.17 * n_hi
    th = P["NOISE_TH"][0] + (P["NOISE_TH"][1] - P["NOISE_TH"][0]) * t
    erosion = smoothstep(th - 0.14, th + 0.12, noise)
    # カリフラワー状の内部構造: 中周波ノイズで密度に凹凸を付ける
    structure = 0.80 + 0.20 * np.clip((n_mid - 0.2) / 0.6, 0.0, 1.0)
    density_e = density * structure
    # 縁ほど侵食を強く効かせる (密度の高い芯は侵食されにくい)
    edge = np.clip(density_e / 1.6, 0.0, 1.0)
    density_e = density_e * (edge + (1.0 - edge) * erosion) * (0.55 + 0.45 * erosion)
    density_e = blur(density_e, CELL / 220.0)

    alpha = 1.0 - np.exp(-density_e * P["ABS"])

    # ── 疑似ボリューム陰影 ─────────────────────────────────────────────────
    trans = self_shadow(density_e, light_dir=(-0.65, -0.75), taps=6,
                        step=max(CELL / 96.0, 1.0), absorb=P["SHADOW"])
    # 上向き法線っぽい明るさ: 密度勾配で上面を明るく
    gy, gx = np.gradient(blur(density_e, CELL / 128.0))
    grad_light = np.clip(0.5 - (gy * 0.65 + gx * 0.55) * 5.0, 0.0, 1.0)
    lit = np.clip(sphere_lit * 1.25 - 0.12, 0.0, 1.0) ** 1.5 * (0.5 + 0.5 * trans ** 1.2) * 0.9 + 0.1 * grad_light
    # 密度の薄い縁は光を通すので少し明るく (縁のレースが暗く沈まないように)
    thin = np.exp(-density_e * 1.2)
    lit = np.clip(lit + thin * 0.25, 0.0, 1.0)
    shade = P["SMOKE_L"][0] + (P["SMOKE_L"][1] - P["SMOKE_L"][0]) * lit
    lum = shade

    # ── 白熱の芯 (爆発のみ) ────────────────────────────────────────────────
    if P["HEAT_T"] > 0:
        heat_w = np.exp(-t / P["HEAT_T"])
        heat = splat_gaussians(CELL, CELL, np.concatenate([px, cpx]), np.concatenate([py, cpy]),
                               np.concatenate([prad * 0.9, crad * 1.1]),
                               np.concatenate([pw * (1.0 - 0.75 * puff_s), cw * 1.3]))
        dist_n = np.sqrt(nx0 ** 2 + (ny0 + rise / CELL * 0.5) ** 2) / max(R, 0.03)
        radial = np.exp(-(dist_n ** 2) * (1.6 + 2.5 * t))
        heat = np.clip(heat * 0.22, 0.0, 1.0) ** 1.2 * heat_w * (0.35 + 0.65 * radial)
        heat = heat * (0.75 + 0.25 * np.clip((n_mid - 0.2) / 0.6, 0, 1)) * (0.6 + 0.4 * sphere_lit)
        lum = lum + heat * 1.25
        alpha = np.clip(alpha + heat * 0.5, 0.0, 1.0)

    # ── 炎のフィラメント ───────────────────────────────────────────────────
    if P["FILAMENTS"] > 0:
        fil = np.zeros((CELL, CELL), dtype=np.float32)
        x1, y1 = fil_pos(t)
        x0, y0 = fil_pos(max(t - 0.075, 0.0))
        life = np.exp(-t / (P["FIL_T"] * 1.35 * fil_life))
        for i in range(P["FILAMENTS"]):
            if life[i] < 0.03:
                continue
            draw_line(fil, cx + x0[i] * CELL, cy + y0[i] * CELL, cx + x1[i] * CELL, cy + y1[i] * CELL,
                      fil_w[i] * CELL / 256.0 * (1.8 + 1.2 * (1 - life[i])), life[i])
        fil = np.clip(fil, 0.0, 1.0)
        glow = blur(fil, CELL / 64.0) * 0.6
        lum = lum + fil * 1.1 + glow * 0.5
        alpha = np.clip(alpha + fil * 0.9 + glow * 0.5, 0.0, 1.0)

    # アルファの立ち上がり (硬いステッカー縁を避ける)
    alpha = smoothstep(0.04, 0.42, alpha) * np.clip(alpha, 0, 1) ** 0.7
    lum = np.where(alpha > 0.002, lum, 0.0)
    rgba = to_rgba(lum, alpha)
    row, col = divmod(f, COLS)
    atlas[row * CELL:(row + 1) * CELL, col * CELL:(col + 1) * CELL] = rgba

save_rgba(OUT, atlas)
preview_over_black(atlas).resize((1024, 1024)).save(OUT.replace(".png", "_preview.png"))
print("wrote", OUT)
