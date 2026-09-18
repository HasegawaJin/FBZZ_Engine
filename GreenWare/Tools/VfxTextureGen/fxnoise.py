"""FBZZ Engine / GreenWare VFX — 手続きテクスチャ生成の共通部品。

numpy だけで動く value-noise / fbm / curl と、ガウス・スプラット、
ストレートアルファ PNG の書き出し。gen_textures.py / gen_flip.py から使う。
"""
import numpy as np
from PIL import Image


# ── 乱数 ───────────────────────────────────────────────────────────────────
def rng(seed):
    return np.random.default_rng(seed)


# ── value noise (2D / 3D, タイル可能) ────────────────────────────────────────
def _smooth(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def value_noise_2d(x, y, period, seed):
    """x, y: 同形配列 (ノイズ空間座標)。period: 整数。格子は period でタイルする。"""
    r = rng(seed)
    lattice = r.random((period, period)).astype(np.float32)
    xi = np.floor(x).astype(np.int64)
    yi = np.floor(y).astype(np.int64)
    xf = _smooth((x - xi).astype(np.float32))
    yf = _smooth((y - yi).astype(np.float32))
    x0 = xi % period
    x1 = (xi + 1) % period
    y0 = yi % period
    y1 = (yi + 1) % period
    a = lattice[y0, x0]
    b = lattice[y0, x1]
    c = lattice[y1, x0]
    d = lattice[y1, x1]
    return (a * (1 - xf) + b * xf) * (1 - yf) + (c * (1 - xf) + d * xf) * yf


def value_noise_3d(x, y, z, period, zperiod, seed):
    r = rng(seed)
    lattice = r.random((zperiod, period, period)).astype(np.float32)
    xi = np.floor(x).astype(np.int64)
    yi = np.floor(y).astype(np.int64)
    zi = np.floor(z).astype(np.int64)
    xf = _smooth((x - xi).astype(np.float32))
    yf = _smooth((y - yi).astype(np.float32))
    zf = _smooth((z - zi).astype(np.float32))
    x0, x1 = xi % period, (xi + 1) % period
    y0, y1 = yi % period, (yi + 1) % period
    z0, z1 = zi % zperiod, (zi + 1) % zperiod

    def lerp_xy(zz):
        a = lattice[zz, y0, x0]
        b = lattice[zz, y0, x1]
        c = lattice[zz, y1, x0]
        d = lattice[zz, y1, x1]
        return (a * (1 - xf) + b * xf) * (1 - yf) + (c * (1 - xf) + d * xf) * yf

    return lerp_xy(z0) * (1 - zf) + lerp_xy(z1) * zf


def fbm_2d(x, y, octaves=5, period=4, seed=0, gain=0.5, lacunarity=2.0):
    """[0,1] 付近に正規化した fbm。x,y は [0,1) の UV。period 単位でタイル。"""
    total = np.zeros_like(x, dtype=np.float32)
    amp = 1.0
    norm = 0.0
    p = period
    for o in range(octaves):
        total += amp * value_noise_2d(x * p, y * p, int(p), seed + o * 101)
        norm += amp
        amp *= gain
        p *= lacunarity
    return total / norm


def fbm_3d(x, y, z, octaves=4, period=4, zperiod=4, seed=0, gain=0.5, lacunarity=2.0):
    total = np.zeros_like(x, dtype=np.float32)
    amp = 1.0
    norm = 0.0
    p = period
    zp = zperiod
    for o in range(octaves):
        total += amp * value_noise_3d(x * p, y * p, z * zp, int(p), int(zp), seed + o * 101)
        norm += amp
        amp *= gain
        p *= lacunarity
        zp *= lacunarity
    return total / norm


def uv_grid(w, h):
    """ピクセル中心の UV [0,1)。返り値 (u, v) いずれも (h, w)。"""
    u = (np.arange(w, dtype=np.float32) + 0.5) / w
    v = (np.arange(h, dtype=np.float32) + 0.5) / h
    return np.meshgrid(u, v)


def centered_grid(w, h):
    """[-1,1] の中心座標 (x, y)。y は上が +。アスペクトは正規化しない。"""
    x = (np.arange(w, dtype=np.float32) + 0.5) / w * 2.0 - 1.0
    y = 1.0 - (np.arange(h, dtype=np.float32) + 0.5) / h * 2.0
    return np.meshgrid(x, y)


# ── 描画部品 ─────────────────────────────────────────────────────────────────
def splat_gaussians(w, h, px, py, radius, weight):
    """ガウス核をたくさん足す (px, py はピクセル座標)。大きさに応じて局所窓で足す。"""
    field = np.zeros((h, w), dtype=np.float32)
    for x, y, r, wgt in zip(px, py, radius, weight):
        if wgt <= 0 or r <= 0.3:
            continue
        rr = int(np.ceil(r * 3.0))
        x0, x1 = int(np.floor(x)) - rr, int(np.floor(x)) + rr + 1
        y0, y1 = int(np.floor(y)) - rr, int(np.floor(y)) + rr + 1
        if x1 <= 0 or y1 <= 0 or x0 >= w or y0 >= h:
            continue
        cx0, cx1 = max(x0, 0), min(x1, w)
        cy0, cy1 = max(y0, 0), min(y1, h)
        gx = (np.arange(cx0, cx1, dtype=np.float32) + 0.5 - x)
        gy = (np.arange(cy0, cy1, dtype=np.float32) + 0.5 - y)
        g = np.exp(-(gy[:, None] ** 2 + gx[None, :] ** 2) / (2.0 * r * r))
        field[cy0:cy1, cx0:cx1] += (wgt * g).astype(np.float32)
    return field


def draw_line(field, x0, y0, x1, y1, width, weight):
    """太さ width [px] の線分を距離場で足す。"""
    h, w = field.shape
    pad = int(np.ceil(width * 2.5)) + 1
    bx0, bx1 = int(min(x0, x1)) - pad, int(max(x0, x1)) + pad + 1
    by0, by1 = int(min(y0, y1)) - pad, int(max(y0, y1)) + pad + 1
    cx0, cx1 = max(bx0, 0), min(bx1, w)
    cy0, cy1 = max(by0, 0), min(by1, h)
    if cx1 <= cx0 or cy1 <= cy0:
        return
    gx = np.arange(cx0, cx1, dtype=np.float32) + 0.5
    gy = np.arange(cy0, cy1, dtype=np.float32) + 0.5
    X, Y = np.meshgrid(gx, gy)
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    if L2 < 1e-6:
        t = np.zeros_like(X)
    else:
        t = np.clip(((X - x0) * dx + (Y - y0) * dy) / L2, 0.0, 1.0)
    px = x0 + t * dx
    py = y0 + t * dy
    d = np.sqrt((X - px) ** 2 + (Y - py) ** 2)
    prof = np.exp(-(d / max(width, 0.3)) ** 2 * 2.0)
    # 線の頭ほど明るく (t=1 が進行方向の先端)
    field[cy0:cy1, cx0:cx1] += (weight * prof * (0.35 + 0.65 * t)).astype(np.float32)


def blur(field, sigma):
    """分離ガウスぼかし (scipy があれば使う)。"""
    try:
        from scipy.ndimage import gaussian_filter
        return gaussian_filter(field, sigma, mode="nearest")
    except Exception:  # pragma: no cover
        return field


def self_shadow(density, light_dir=(-0.6, -0.8), taps=6, step=3.0, absorb=0.08):
    """密度を光源方向へ行進して自己遮蔽を求める。返り値は透過率 [0,1]。"""
    h, w = density.shape
    trans = np.ones_like(density)
    acc = np.zeros_like(density)
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    for i in range(1, taps + 1):
        sx = xs + light_dir[0] * step * i
        sy = ys + light_dir[1] * step * i
        sxi = np.clip(sx.astype(np.int64), 0, w - 1)
        syi = np.clip(sy.astype(np.int64), 0, h - 1)
        acc += density[syi, sxi]
    trans = np.exp(-acc * absorb * step)
    return trans


def to_rgba(lum, alpha):
    """輝度 (無彩色) とストレートアルファから RGBA uint8 を作る。"""
    lum = np.clip(lum, 0.0, 1.0)
    alpha = np.clip(alpha, 0.0, 1.0)
    l8 = (lum * 255.0 + 0.5).astype(np.uint8)
    a8 = (alpha * 255.0 + 0.5).astype(np.uint8)
    return np.dstack([l8, l8, l8, a8])


def save_rgba(path, rgba):
    Image.fromarray(rgba, "RGBA").save(path, optimize=True)


def preview_over_black(rgba):
    """アルファを乗じて黒の上に合成したプレビュー (RGB)。"""
    a = rgba[..., 3:4].astype(np.float32) / 255.0
    rgb = rgba[..., :3].astype(np.float32) * a
    return Image.fromarray(rgb.astype(np.uint8), "RGB")


def splat_lit_spheres(w, h, px, py, radius, weight, light=(-0.55, -0.6, 0.58), softness=1.0):
    """球として陰影を付けたガウス核を足す。

    返り値 (density, lit)。lit は手前の球が奥を隠す形で合成した明るさ [0,1]。
    小さな球をたくさん重ねると、煙のカリフラワー状の陰影が出る。
    """
    density = np.zeros((h, w), dtype=np.float32)
    lit_sum = np.zeros((h, w), dtype=np.float32)
    lx, ly, lz = light
    for x, y, r, wgt in zip(px, py, radius, weight):
        if wgt <= 0 or r <= 0.3:
            continue
        rr = int(np.ceil(r * 2.6))
        x0, x1 = int(np.floor(x)) - rr, int(np.floor(x)) + rr + 1
        y0, y1 = int(np.floor(y)) - rr, int(np.floor(y)) + rr + 1
        if x1 <= 0 or y1 <= 0 or x0 >= w or y0 >= h:
            continue
        cx0, cx1 = max(x0, 0), min(x1, w)
        cy0, cy1 = max(y0, 0), min(y1, h)
        gx = (np.arange(cx0, cx1, dtype=np.float32) + 0.5 - x) / r
        gy = (np.arange(cy0, cy1, dtype=np.float32) + 0.5 - y) / r
        GX, GY = np.meshgrid(gx, gy)
        d2 = GX * GX + GY * GY
        g = np.exp(-d2 / (2.0 * softness * softness))
        nz = np.sqrt(np.clip(1.6 - d2, 0.0, None))
        nl = np.sqrt(GX * GX + GY * GY + nz * nz) + 1e-6
        lit = np.clip(0.5 + 0.5 * (GX * lx + GY * ly + nz * lz) / nl, 0.0, 1.0)
        density[cy0:cy1, cx0:cx1] += (wgt * g).astype(np.float32)
        # 手前の球が奥の球を隠す (over 合成)。平均にすると陰影が溶けて平板になる。
        cov = np.clip(g * wgt * 1.6, 0.0, 1.0) * np.clip(1.4 - d2, 0.0, 1.0)
        lit_sum[cy0:cy1, cx0:cx1] = lit_sum[cy0:cy1, cx0:cx1] * (1.0 - cov) + lit * cov
    return density, lit_sum
