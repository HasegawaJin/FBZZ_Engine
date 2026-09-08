"""双剣の斬撃 SE を作り直す。

WHY 作り直すか (2026-09-08 の実測):
    ヒット      クレスト 9.8〜11.1 dB / 重心 2.0〜2.3kHz
    振り        立ち上がり 99 / 194 ms
  打撃音の «気持ちよさ» はほぼクレストファクタ (瞬間最大 ÷ 実効値) で決まる。
  10dB は «潰れた雑音» の値で、刃が当たった «点» が耳に立たない。振りも同じで、
  立ち上がりが 100ms を超えると «山» が無く、振り抜いた瞬間が音に出ない。

設計:
  振り  = 空気の唸りを «振り抜きの位置» で最大にし、そこから 40ms で切る。
          周波数も同時に下げる (通り過ぎるので下がる)。
  ヒット= 3 層。① 1〜2ms の破裂 (刃が食い込む «点») ② 刀身の鳴き ③ 低い衝撃。
          ①を他より 14dB 以上高く置くことでクレストを稼ぐ。

音程 (README_v3_Blades_Arena.md の表):
  右剣 220Hz / 左剣 660Hz。ボス 55Hz の 2 / 3 オクターブ上に逃がしてある。
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import sfxlib as S

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "blade")
os.makedirs(OUT, exist_ok=True)
R, L = 220.0, 660.0

def crest_db(x):
    pk = float(np.max(np.abs(x))); rms = float(np.sqrt(np.mean(x ** 2)))
    return 20 * np.log10(pk / (rms + 1e-12))

# ── 振り抜き ────────────────────────────────────────────────────────────────
def swing(base, stage, var):
    """stage 0..2 = 連撃の段。段が上がるほど速く・低く・短くする。"""
    dur   = (0.30, 0.27, 0.34)[stage]
    peak  = (0.72, 0.70, 0.66) [stage]          # 山を置く位置 (尺に対する比)
    speed = (1.00, 1.14, 1.32) [stage]          # 振りの速さ
    seed  = 900 + stage * 10 + var
    n = int(S.SR * dur)
    x = S.t(dur)
    k = np.linspace(0.0, 1.0, n)

    # ① 空気。山を peak に置き、そこから急に落とす (振り抜いた «後» は鳴らない)。
    rise = np.clip(k / peak, 0, 1) ** 2.2
    fall = np.clip((1 - k) / (1 - peak), 0, 1) ** 1.5
    envA = rise * fall
    # WHY 1 極では足りないか: 6dB/oct だと 10kHz 台が残って «シャーッ» と鳴る。
    #     元の素材は重心 3.1〜3.7kHz で、そこが «風を切った» の帯域。4 極で落とす。
    air = S.band(S.noise(dur, seed), 380, 4200, poles=4)
    # 通過に伴って中心が下がる。上から下へ抜ける «ドップラー» が振りの正体。
    hi = S.band(S.noise(dur, seed + 1), 1800, 5200, poles=4)
    lo = S.band(S.noise(dur, seed + 2), 520, 1900, poles=4)
    blend = np.clip((k - peak * 0.45) / 0.5, 0, 1)
    body = hi * (1 - blend) + lo * blend
    x_out = (air * 0.35 + body * 0.85) * envA

    # ② 刀身が唸る。刃が薄いので «鳴き» は倍音が高い所に 2 本だけ。
    for mul, amp in ((3.0, 0.16), (7.3, 0.09)):
        f0 = base * mul * speed
        ring = S.sweep(f0 * 1.06, f0 * 0.94, dur, amp=amp)
        x_out += ring * envA ** 1.4

    # ③ 振り出しの «衣擦れ»。ここだけ頭に置くと «構えて振った» の 2 拍になる。
    st = S.band(S.noise(0.045, seed + 3), 1500, 5600, poles=3) * 0.22
    st *= S.env(len(st), 0.002, 0.040, curve=2.0)
    S.place(x_out, st, 0.0)

    y = S.arena(x_out, open_sky=True, wet=0.16)
    # 尻尾は要らない。次の段が 0.25 秒で来るので、部屋鳴りは 2 反射で切る。
    m = int(S.SR * (dur + 0.14))
    y = y[:m]
    y[-int(S.SR*0.05):] *= np.linspace(1.0, 0.0, int(S.SR*0.05)) ** 1.4
    return S.normalize(y, 0.72 + 0.06 * stage)

# ── 当たり ──────────────────────────────────────────────────────────────────
def hit(base, var, heavy=False):
    """① 破裂 ② 刀身の鳴き ③ 低い衝撃。①を突出させてクレストを稼ぐ。"""
    dur  = 0.62 if heavy else 0.44
    seed = 1200 + var + (50 if heavy else 0)
    n = int(S.SR * dur)
    x = S.silence(dur)

    # ① 破裂 — 1.4ms で立ち上がって 6ms で消える。**ここが «当たった点»。**
    crack = S.noise(0.012, seed) * 1.0
    # 破裂も 12kHz で切る。ここを開けたままだと «紙を裂いた» に寄る。
    crack = S.band(crack, 2600, 11000, poles=3)
    crack *= S.env(len(crack), 0.0004, 0.0075, curve=3.2)
    S.place(x, crack * (0.72 if heavy else 0.62), 0.0)
    # 金属どうしの «キン» を 1 本だけ重ねる (破裂の中に音程を 1 つ置く)
    ping = S.tone(base * (12.0 if base < 400 else 6.5), 0.05, 0.22)
    ping *= S.env(len(ping), 0.0006, 0.045, curve=3.0)
    S.place(x, ping, 0.0008)

    # ② 刀身の鳴き。右は太く低く、左は細く高く。
    ring = S.metal(0.34 if heavy else 0.26, seed + 1, base * 2.0,
                   partials=7 if heavy else 5, decay=0.085 if heavy else 0.055, spread=1.42)
    S.place(x, S.lp(ring, 6000, 2) * (0.62 if heavy else 0.52), 0.0016)

    # ③ 低い衝撃 — «重さ»。装甲が凹む音なので 66〜88Hz。
    thump = S.sweep(96.0 if heavy else 118.0, 62.0, 0.20, amp=1.0)
    thump *= S.env(int(S.SR * 0.20), 0.0018, 0.185, curve=2.6)
    S.place(x, thump * (0.92 if heavy else 0.70), 0.0024)

    # ④ 破片。当たった «後» に散る細かい粒 (heavy だけ)。
    if heavy:
        deb = S.band(S.noise(0.22, seed + 2), 1400, 6500, poles=3) * 0.22
        deb *= S.env(len(deb), 0.010, 0.205, curve=2.2)
        S.place(x, deb, 0.030)

    y = S.arena(x, open_sky=True, wet=0.20 if heavy else 0.15)
    m = int(S.SR * (dur + (0.34 if heavy else 0.18)))
    y = y[:m]
    y[-int(S.SR*0.06):] *= np.linspace(1.0, 0.0, int(S.SR*0.06)) ** 1.4
    return S.normalize(y, 0.90 if heavy else 0.84)

def charge_slash(base):
    """溜め斬り。振りを長く引いて、当たりを重くしたもの。"""
    x = swing(base, 2, 0) * 0.9
    h = hit(base, 7, heavy=True)
    # silence() は «秒» を取る。サンプル数を渡さないこと。
    out = S.silence(max(len(x) / S.SR, 0.255 + len(h) / S.SR))
    S.place(out, x, 0.0)
    S.place(out, h * 1.0, 0.255)
    return S.normalize(out, 0.94)

if __name__ == "__main__":
    jobs = []
    for side, base in (("R", R), ("L", L)):
        for si, sname in enumerate(("1st", "2nd", "3rd")):
            for v in (1, 2):
                jobs.append((f"SE_BLD_Swing_{side}_{sname}_{v:02d}.wav", swing(base, si, v)))
        for v in (1, 2, 3):
            jobs.append((f"SE_BLD_Hit_{side}_{v:02d}.wav", hit(base, v)))
        jobs.append((f"SE_BLD_Hit_Finish_{side}.wav", hit(base, 9, heavy=True)))
        jobs.append((f"SE_BLD_ChargeSlash_{side}.wav", charge_slash(base)))
    for name, wav in jobs:
        S.write(os.path.join(OUT, name), wav)
    print(f"{len(jobs)} 本\n")
    for name, wav in jobs[:6] + jobs[6:9] + [jobs[9]]:
        print(f"  {name:<30} {len(wav)/S.SR:.3f}s  crest {crest_db(wav):5.1f}dB")
