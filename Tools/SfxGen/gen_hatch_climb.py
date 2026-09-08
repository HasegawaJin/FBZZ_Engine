"""蓋 (コアハッチ) の開閉と、登攀の手掛けを合成する。

WHY この 3 種だけ新規に要るか:
  既存の 180 本を精査したところ、鳴らす側が無いだけのバンクは «音源はある» ので
  配線で済む。音そのものが 1 本も無いのは «蓋が開く / 閉じる» と «脚を掴む» の
  3 つだけだった (Docs/climb-core.md の中核が丸ごと無音だった)。

音程の置き方 (README_v3 の表に従う):
  ボス 55Hz(A1) / 右剣 220Hz / 左剣 660Hz。蓋はボスの体なので 55Hz 系へ、
  手掛けはプレイヤーの体なので剣より上の 520〜700Hz へ置いて、
  同時に鳴っても濁らないようにする。
"""
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
def climb_grab(i):
    """WHY 3 変奏を層化するか: 登りは 0.83 秒周期で手が変わるので、同じ音が
       すぐ返ってくる。乱数任せだと似た 2 つが並んだ瞬間に «使い回し» が出る。
       接触音の基音・擦れの量・サーボの長さ・焼き込む音量を等分して 1 つずつ配る。"""
    base   = (520.0, 605.0, 690.0)[i]          # 接触する装甲の «厚み»
    scrape = (0.34, 0.20, 0.27)[i]             # 擦れの量
    pull   = (0.115, 0.150, 0.132)[i]          # 荷重が乗るまでの長さ
    gain   = (0.81, 1.00, 0.90)[i]             # -1.8 / 0.0 / -0.9 dB を焼き込む
    dur = 0.26
    x = S.silence(dur)
    # ① 手が装甲に当たる。指が先に触れて掌が遅れる (2 段)。
    S.place(x, S.metal(0.10, 61 + i * 3, base, partials=4, decay=0.028) * 0.50, 0.000)
    S.place(x, S.metal(0.12, 62 + i * 3, base * 0.72, partials=5, decay=0.040) * 0.34, 0.011)
    # ② 掴んで擦れる。短い雑音を帯域で削って «金属の上を滑った» にする。
    sc = S.onepole_hp(S.noise(0.07, 63 + i * 3), 1800) * scrape
    sc *= S.env(len(sc), 0.004, 0.05, curve=1.8)
    S.place(x, S.onepole_lp(sc, 7000), 0.008)
    # ③ 腕のサーボに荷重が乗る。下がる ─ 支えた側の音。
    S.place(x, S.servo(pull, 64 + i * 3, f0=205, f1=142, amp=0.22, rough=0.22), 0.016)
    # ④ 関節が収まる細かいガタつき。
    S.place(x, S.metal(0.06, 65 + i * 3, base * 1.9, partials=3, decay=0.018) * 0.13, 0.055 + pull * 0.4)
    y = S.arena(x, open_sky=True, wet=0.12) * gain
    # WHY 尻尾を切るか: 手掛けは 0.83 秒周期で返ってくる。部屋の残響をそのまま
    #     残すと次の一手に被って «ずっと擦れている» になる。反射 2 つ分で切る。
    n = int(S.SR * 0.45)
    y = y[:n] * np.concatenate([np.ones(n - int(S.SR*0.06)),
                                np.linspace(1.0, 0.0, int(S.SR*0.06)) ** 1.5])
    return y

if __name__ == "__main__":
    jobs = [("Boss/SE_BOSS_Hatch_Open.wav",  hatch_open(),  0.86),
            ("Boss/SE_BOSS_Hatch_Close.wav", hatch_close(), 0.86)]
    for i in range(3):
        jobs.append((f"Player/SE_PL_Climb_Grab_{i+1:02d}.wav", climb_grab(i), None))
    # 手掛けの 3 本は «変奏どうしの音量差» が意味を持つので、まとめて 1 つの
    # 係数で正規化する (個別に正規化すると焼き込んだ差が消える)。
    grabs = [w for n, w, p in jobs if "Climb_Grab" in n]
    gm = max(float(np.max(np.abs(w))) for w in grabs)
    for name, wav, peak in jobs:
        path = os.path.join(OUT, name.replace("/", "_"))
        out = S.normalize(wav, peak) if peak else wav * (0.82 / gm)
        sec = S.write(path, out)
        print(f"  {name:<38} {sec:.3f}s  peak {float(np.max(np.abs(out))):.3f}")
