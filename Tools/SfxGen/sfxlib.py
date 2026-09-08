"""GreenWare SFX 合成の共通部。

48kHz / 16bit / モノラル。録音素材は使わず、すべて手続き的に組む
(Assets/Sound/SE/README_Others.md, README_v3_Blades_Arena.md と同じ前提)。

WHY アリーナの反射を全部の音に通すか:
  この作品の «広い場所に居る» は残響そのものではなく «最初の反射が返るまでの間» で
  出ている。屋根の輪 (内半径 17.3m / 高さ 6m) までの往復 36.6m = 107ms、
  外壁 (床の縁 40m) 往復 80m = 233ms。107ms まで何も返らない間が部屋の広さになる。
  ボスは中央に立つので、屋根が抜けている中央設定 (open_sky) で書き出す。
"""
import numpy as np

SR = 48000

def _rng(seed):
    return np.random.default_rng(seed)

def t(dur):
    return np.arange(int(SR * dur)) / SR

def silence(dur):
    return np.zeros(int(SR * dur))

def place(dst, src, at):
    """dst の at 秒へ src を足し込む。はみ出しは切る。"""
    i = int(at * SR)
    if i >= len(dst): return dst
    n = min(len(src), len(dst) - i)
    dst[i:i + n] += src[:n]
    return dst

def env(n, attack, decay, hold=0.0, curve=2.0):
    """区分的な包絡。attack/hold/decay はすべて秒。"""
    a = max(int(attack * SR), 1)
    h = int(hold * SR)
    d = max(int(decay * SR), 1)
    e = np.zeros(n)
    e[:min(a, n)] = np.linspace(0.0, 1.0, min(a, n)) ** 0.6
    if h and a < n:
        e[a:min(a + h, n)] = 1.0
    s = a + h
    if s < n:
        m = min(d, n - s)
        e[s:s + m] = np.linspace(1.0, 0.0, m) ** curve
    return e

def tone(freq, dur, amp=1.0, detune=0.0, phase=0.0):
    x = t(dur)
    f = freq * (1.0 + detune * np.linspace(0.0, 1.0, len(x)))
    return amp * np.sin(2 * np.pi * np.cumsum(f) / SR + phase)

def sweep(f0, f1, dur, amp=1.0, log=True):
    x = t(dur)
    k = np.linspace(0.0, 1.0, len(x))
    f = f0 * (f1 / f0) ** k if log else f0 + (f1 - f0) * k
    return amp * np.sin(2 * np.pi * np.cumsum(f) / SR)

def noise(dur, seed, amp=1.0):
    return amp * _rng(seed).standard_normal(int(SR * dur))

def onepole_lp(x, cutoff):
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i, v in enumerate(x):
        acc = a * acc + (1 - a) * v
        y[i] = acc
    return y

def onepole_hp(x, cutoff):
    return x - onepole_lp(x, cutoff)

def lp(x, cutoff, poles=4):
    """1 極を重ねた低域通過。1 極 (6dB/oct) では «上が残って» シャーッと鳴る。"""
    y = x
    for _ in range(poles):
        y = onepole_lp(y, cutoff)
    return y

def hp(x, cutoff, poles=2):
    y = x
    for _ in range(poles):
        y = onepole_hp(y, cutoff)
    return y

def band(x, lo_hz, hi_hz, poles=3):
    return hp(lp(x, hi_hz, poles), lo_hz, max(poles - 1, 1))

def resonator(x, freq, q):
    """2 次共振。金属の «鳴き» を 1 本足すのに使う。"""
    w = 2 * np.pi * freq / SR
    r = np.exp(-w / (2 * q))
    a1, a2 = 2 * r * np.cos(w), -r * r
    y = np.zeros_like(x)
    for i in range(len(x)):
        acc = x[i]
        if i >= 1: acc += a1 * y[i - 1]
        if i >= 2: acc += a2 * y[i - 2]
        y[i] = acc
    return y * (1 - r)

def metal(dur, seed, base, partials=6, decay=0.10, spread=1.7):
    """叩かれた金属板。非調和な部分音を重ねる (装甲・蓋の花弁に使う)。"""
    r = _rng(seed)
    out = silence(dur)
    for k in range(partials):
        f = base * (1.0 + spread * k) * (1.0 + r.uniform(-0.04, 0.04))
        if f > SR * 0.45: break
        d = decay * (0.45 ** (k / max(partials - 1, 1)))
        seg = tone(f, dur, amp=0.9 ** k) * env(int(SR * dur), 0.0008, d, curve=2.4)
        out += seg
    return out

def servo(dur, seed, f0=180.0, f1=240.0, amp=0.5, rough=0.35):
    """サーボの唸り。基音のゆらぎと軸受のざらつきで «動いている» を出す。"""
    r = _rng(seed)
    x = t(dur)
    wob = 1.0 + 0.02 * np.sin(2 * np.pi * 7.3 * x + r.uniform(0, 6.28))
    f = (f0 * (f1 / f0) ** np.linspace(0, 1, len(x))) * wob
    body = np.sin(2 * np.pi * np.cumsum(f) / SR)
    body += 0.35 * np.sin(4 * np.pi * np.cumsum(f) / SR)
    grit = onepole_hp(noise(dur, seed + 1), 900) * rough
    return amp * (body * 0.8 + grit) * env(len(x), 0.012, 0.05, hold=max(dur - 0.07, 0.0), curve=1.4)

def arena(x, open_sky=True, wet=0.34, seed=7):
    """アリーナの部屋。実測のタップ位置へ反射を置き、その後を密度で埋める。

    中央 (屋根が抜けている) は後部残響が短く暗い。ボスは中央に立つ。"""
    taps = ([0.104, 0.116, 0.128, 0.152, 0.168, 0.232, 0.268] if open_sky
            else [0.104, 0.116, 0.128, 0.152, 0.168, 0.188, 0.200])
    gains = [0.42, 0.34, 0.30, 0.24, 0.21, 0.16, 0.13]
    tail = 0.9 if open_sky else 1.5
    out = np.concatenate([x, silence(tail)])
    for tp, g in zip(taps, gains):
        place(out, onepole_lp(x, 4200) * g, tp)
    # 後部残響: 指数減衰する雑音で密度だけ足す
    r = _rng(seed)
    n = len(out)
    late = onepole_lp(r.standard_normal(n), 2600 if open_sky else 1800)
    decay = np.exp(-np.arange(n) / (SR * (0.26 if open_sky else 0.42)))
    late *= decay * 0.06
    # 入力の包絡で late を励起する (無音のところで鳴らない)
    e = onepole_lp(np.abs(np.concatenate([x, silence(tail)])), 40)
    e /= (e.max() + 1e-9)
    out += late * e
    return (1.0 - wet) * np.concatenate([x, silence(tail)]) + wet * out

def normalize(x, peak=0.89):
    m = np.max(np.abs(x))
    return x if m < 1e-9 else x * (peak / m)

def write(path, x, gain=1.0):
    import wave, struct
    y = np.clip(x * gain, -1.0, 1.0)
    data = (y * 32767.0).astype("<i2").tobytes()
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(data)
    return len(y) / SR
