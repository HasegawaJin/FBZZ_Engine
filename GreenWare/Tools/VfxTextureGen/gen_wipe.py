"""FBZZ Engine / GreenWare — シーン遷移のワイプ用マスク (ScreenWipe.hlsl の t0)。

    OUT=... python gen_wipe.py

R = その画素が «塗られる順» 0..1 (0 が最初、1 が最後)。G/B は R と同じ、A = 1。
"""
import os
import numpy as np
from fxnoise import rng, fbm_2d, uv_grid, blur, save_rgba

OUT = os.environ.get("OUT", ".")
SEED = int(os.environ.get("SEED", "5"))
r = rng(SEED)
N = 1024


def write(name, order):
    order = np.clip(order, 0.0, 1.0)
    v = (order * 255.0 + 0.5).astype(np.uint8)
    rgba = np.dstack([v, v, v, np.full_like(v, 255)])
    save_rgba(os.path.join(OUT, name), rgba)
    print("wrote", name)


def voronoi_cells(u, v, count, seed):
    """最も近い母点の番号と、境界までの距離 (2 番目との差)。"""
    rr = rng(seed)
    pts = rr.random((count, 2)).astype(np.float32)
    best = np.full(u.shape, 1e9, np.float32)
    second = np.full(u.shape, 1e9, np.float32)
    idx = np.zeros(u.shape, np.int32)
    for i, (px, py) in enumerate(pts):
        d = (u - px) ** 2 + (v - py) ** 2
        closer = d < best
        second = np.where(closer, best, np.minimum(second, d))
        idx = np.where(closer, i, idx)
        best = np.where(closer, d, best)
    return idx, np.sqrt(second) - np.sqrt(best), pts


# ── 1. T_Wipe_Shards — 装甲片が斜めに剥がれていく ─────────────────────────────
# 左下 → 右上へ走る斜めの前線。前線は破片 (ボロノイ) ごとに段差を持ち、破片の中は
# 前線と平行な向きに少し傾く (1 枚ずつ «めくれる»)。縁は細かいノイズでギザギザ。
def shards():
    u, v = uv_grid(N, N)
    ang = np.deg2rad(28.0)
    base = (u * np.cos(ang) + (1.0 - v) * np.sin(ang))          # 斜めの進み
    base = (base - base.min()) / (base.max() - base.min())
    idx, edge, pts = voronoi_cells(u, v, 140, SEED + 1)
    rr = rng(SEED + 2)
    cell_off = rr.uniform(-0.06, 0.06, 140).astype(np.float32)   # 破片ごとの段差
    cell_tilt = rr.uniform(-0.04, 0.04, 140).astype(np.float32)  # 破片の中の傾き
    within = (u - pts[idx, 0]) * np.cos(ang + np.pi / 2) + (v - pts[idx, 1]) * np.sin(ang + np.pi / 2)
    order = base + cell_off[idx] + within * cell_tilt[idx] * 6.0
    # 破片の境界は一段遅れる (境目が最後まで残って «割れ目» として見える)
    seam = np.exp(-edge / 0.006) * 0.05
    order = order + seam
    n = fbm_2d(u, v, octaves=4, period=24, seed=SEED + 3)
    order = order + (n - 0.5) * 0.03
    order = (order - order.min()) / (order.max() - order.min())
    write("T_Wipe_Shards.png", order)


# ── 2. T_Wipe_Iris — 六角の絞り。中心から外へ (入) / 外から中心へ (出) ────────────
def iris():
    u, v = uv_grid(N, N)
    x, y = (u - 0.5) * 2.0, (v - 0.5) * 2.0
    d = np.sqrt(x * x + y * y)
    # 六角格子のセルごとに少し遅らせて «絞りの羽» にする
    hx = x * 9.0
    hy = y * 9.0
    # 六角セルの中心 (axial 近似): 平行四辺形格子で量子化
    qx = np.floor(hx / 1.5)
    qy = np.floor((hy - (qx % 2) * 0.75) / 1.5)
    cell_hash = np.mod(np.sin(qx * 12.9898 + qy * 78.233) * 43758.5453, 1.0)
    order = d / 1.42 + (cell_hash - 0.5) * 0.06
    n = fbm_2d(u, v, octaves=3, period=12, seed=SEED + 7)
    order = order + (n - 0.5) * 0.02
    order = (order - order.min()) / (order.max() - order.min())
    write("T_Wipe_Iris.png", order)


# ── 3. T_Wipe_Scan — 走査線。上から下へ、行ごとにばらつく ───────────────────────
def scan():
    u, v = uv_grid(N, N)
    rows = np.floor(v * 72.0)
    rr = rng(SEED + 9)
    row_off = rr.uniform(-0.08, 0.08, 73).astype(np.float32)
    order = v + row_off[rows.astype(int)] + (u - 0.5) * 0.04 * np.where(rows % 2 == 0, 1.0, -1.0)
    n = fbm_2d(u, v, octaves=3, period=40, seed=SEED + 10)
    order = order + (n - 0.5) * 0.015
    order = (order - order.min()) / (order.max() - order.min())
    write("T_Wipe_Scan.png", order)


if __name__ == "__main__":
    shards()
    iris()
    scan()
