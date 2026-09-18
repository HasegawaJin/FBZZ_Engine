"""コアハッチの開閉。登攀音はgen_movement.pyへ移管済み。"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import sfxlib as S

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(OUT, exist_ok=True)

A1 = 55.0

# ── 蓋を開く (Boss_Hatch_Open 0.63s) ────────────────────────────────────────
def hatch_open():
    dur = 0.72
    x = S.silence(dur)
    # ① 4 枚の掛け金が外れる。花弁は R/F/L/B の順に付いているので、
    #    同時ではなく 18ms ずつずらす ─ 揃えると «1 枚の板» に聞こえる。
    for k, at in enumerate((0.000, 0.018, 0.034, 0.049)):
        clack = S.metal(0.16, 11 + k, A1 * 6 * (1.0 + 0.05 * k), partials=5, decay=0.045)
        S.place(x, clack * (0.55 - 0.06 * k), at)
    # ② 花弁が 75 度開く。サーボは上がりながら軽くなる (負荷が抜ける)。
    S.place(x, S.servo(0.46, 21, f0=165, f1=262, amp=0.30), 0.05)
    # ③ コアがせり上がる。55Hz から完全五度上へ ─ «出てきた» を音程で言う。
    rise = S.sweep(A1, A1 * 1.5, 0.40, amp=0.42)
    rise *= S.env(len(rise), 0.10, 0.16, hold=0.14, curve=1.6)
    rise += S.sweep(A1 * 2, A1 * 3, 0.40, amp=0.12) * S.env(len(rise), 0.12, 0.18, hold=0.10)
    S.place(x, rise, 0.20)
    # ④ 花弁が開ききって止まる。2 枚ずつ受け止まるので 2 度鳴る。
    for k, at in enumerate((0.575, 0.596)):
        S.place(x, S.metal(0.20, 31 + k, A1 * 4, partials=4, decay=0.075) * (0.40 - 0.08 * k), at)
    # ⑤ 露出したコアの唸り。ここから «叩ける» が続くので、切らずに残す。
    hum = (S.tone(A1 * 1.5, 0.30, 0.10) + S.tone(A1 * 3, 0.30, 0.045))
    S.place(x, hum * S.env(len(hum), 0.06, 0.22, curve=1.2), 0.42)
    return S.arena(x, open_sky=True, wet=0.34)

# ── 蓋を閉じる (Boss_Hatch_Close 0.50s) ─────────────────────────────────────
def hatch_close():
    dur = 0.60
    x = S.silence(dur)
    # ① サーボが下がる。閉じる側は重力に逆らわないので短く、低く終わる。
    S.place(x, S.servo(0.34, 41, f0=250, f1=148, amp=0.26), 0.010)
    # ② コアが沈む。開くときの逆 ─ 五度上から基音へ戻す。
    sink = S.sweep(A1 * 1.5, A1, 0.30, amp=0.34)
    sink *= S.env(len(sink), 0.04, 0.20, hold=0.06, curve=1.8)
    S.place(x, sink, 0.02)
    # ③ 掛け金が噛む。**窓が閉じた合図**なので、開くときより重く、1 発で決める。
    latch = S.metal(0.28, 51, A1 * 4.5, partials=7, decay=0.10) * 0.62
    S.place(x, latch, 0.435)
    thud = S.tone(A1, 0.26, 0.40) * S.env(int(S.SR * 0.26), 0.002, 0.24, curve=2.6)
    thud += S.tone(A1 * 0.5, 0.26, 0.22) * S.env(int(S.SR * 0.26), 0.004, 0.25, curve=2.2)
    S.place(x, thud, 0.437)
    return S.arena(x, open_sky=True, wet=0.30)

# ── 脚を掴む (Player_Climb_Grab 3 変奏) ─────────────────────────────────────
if __name__ == "__main__":
    jobs = [("Boss/SE_BOSS_Hatch_Open.wav",  hatch_open(),  0.86),
            ("Boss/SE_BOSS_Hatch_Close.wav", hatch_close(), 0.86)]
    for name, wav, peak in jobs:
        path = os.path.join(OUT, name.replace("/", "_"))
        out = S.normalize(wav, peak)
        sec = S.write(path, out)
        print(f"  {name:<38} {sec:.3f}s  peak {float(np.max(np.abs(out))):.3f}")
