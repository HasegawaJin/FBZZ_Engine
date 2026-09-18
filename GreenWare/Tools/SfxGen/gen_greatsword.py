"""両手剣: 接触、金属の抵抗、抜けを分けた手続き合成。録音素材不使用。

python gen_greatsword.py --output <Weapon directory> [--preview <wav>]
既存metaのGUIDは保存する。旧素材の削除は参照監査を伴う別作業。
"""
from pathlib import Path
import argparse
import json
import uuid
import wave
import numpy as np

SR = 48000


def time(seconds):
    return np.arange(round(seconds * SR)) / SR


def unit(x):
    return x / max(float(np.sqrt(np.mean(x * x))), 1e-12)


def noise(seconds, seed, low, high):
    rng = np.random.default_rng(seed)
    x = rng.standard_normal(len(time(seconds)))
    f = np.fft.rfftfreq(len(x), 1 / SR)
    band = (1 - np.exp(-(f / low) ** 4)) * np.exp(-(f / high) ** 4)
    return unit(np.fft.irfft(np.fft.rfft(x) * band, n=len(x)))


def envelope(t, attack, decay):
    return (1 - np.exp(-t / attack)) * np.exp(-t / decay)


def modes(t, seed, base, decay, count=11):
    rng = np.random.default_rng(seed)
    result = np.zeros_like(t)
    # 非整数比の部分音と周波数ごとの減衰で、電子的な単音を避ける。
    for i in range(count):
        f = base * (1 + i * 1.37 + i * i * 0.073) * rng.uniform(.96, 1.04)
        if f > 8500:
            break
        result += np.sin(2 * np.pi * f * t + rng.uniform(0, 2*np.pi) + .7 * np.exp(-t / .012)) * (
            .82 ** i * envelope(t, .0005, decay / (1 + .23 * i)))
    return result


def add(dst, src, seconds):
    offset = round(seconds * SR)
    count = min(len(src), len(dst) - offset)
    if count > 0:
        dst[offset:offset + count] += src[:count]


def finish(x, peak_db=-5, loop=False):
    x = x - np.mean(x)
    if not loop:
        n = min(192, len(x) // 4)
        x[:24] *= np.linspace(0, 1, 24)
        x[-n:] *= np.linspace(1, 0, n) ** 2
    # 層の釣り合いを先に整える。瞬間ピークだけを増幅しない。
    x *= 10 ** (peak_db / 20) / max(np.max(np.abs(x)), 1e-12)
    assert np.isfinite(x).all() and np.max(np.abs(x)) < .9
    return x


def swing(kind, variant):
    duration = dict(Down=.22, Up=.23, Spin=.30, Slide=.25, Air=.26, Finish=.30, Charge=.34)[kind]
    t = time(duration)
    seed = 3100 + list(('Down','Up','Spin','Slide','Air','Finish','Charge')).index(kind) * 20 + variant
    # 通過の山は全種類60ms。ゲーム側の予約時刻もこの値に合わせる。
    shape = np.exp(-.5 * ((t - .060) / .028) ** 2)
    low = noise(duration, seed, 180, 1800)
    high = noise(duration, seed + 1, 1500, 6000)
    sweep = np.clip(t / .10, 0, 1)
    if kind == 'Up':
        sweep = 1 - sweep
    x = (.16 * high * (1 - sweep) + .26 * low * sweep) * shape
    x += .11 * np.sin(2*np.pi*(370*t + 500*t*t)) * shape
    if kind in ('Spin', 'Charge'):
        x += .10 * low * np.exp(-.5 * ((t - .16) / .042) ** 2)
    if kind in ('Air', 'Finish', 'Charge'):
        x += .16 * np.sin(2*np.pi*(145*t - 100*t*t)) * shape
    return finish(x, -9 if kind not in ('Finish','Charge') else -7)


def impact(variant, heavy=False, execute=False):
    duration = .88 if execute else .48 if heavy else .32
    t = time(duration)
    seed = 4200 + variant * 17 + 200 * heavy + 400 * execute
    contact = noise(duration, seed, 1500, 7800) * envelope(t, .0006, .010)
    # 摩擦は短く、接触後に抵抗を押し切る。ノイズへ不規則な粒を加える。
    grit = noise(duration, seed+1, 650, 4800)
    modulation = .65 + .35*np.sin(2*np.pi*(85*t+110*t*t)) ** 2
    scrape = grit * envelope(t, .004, .038 if not heavy else .060) * modulation
    plate = modes(t, seed+2, 295 if not heavy else 205, .065 if not heavy else .095)
    frequency = 74 + 90*np.exp(-t/.018)
    body = np.sin(2*np.pi*np.cumsum(frequency)/SR) * envelope(t, .0015, .065 if not heavy else .11)
    x = .23*contact + .30*scrape + .15*plate + (.48 if not heavy else .64)*body
    if execute:
        fracture = modes(t, seed+4, 167, .15) * .25
        fracture += .26*noise(duration,seed+5,350,3600)*envelope(t,.002,.045)
        add(x, fracture, .095)
        for j, at in enumerate((.18,.245,.34)):
            add(x, modes(t,seed+10+j,520+170*j,.025)*.055, at)
    return finish(x, -3.5 if execute else -6.5 if heavy else -7.5)


def parry(variant, just=False, guard=False):
    t=time(.27 if guard else .56 if just else .38)
    seed=5300+variant*17
    x=.28*noise(len(t)/SR,seed,800,6200)*envelope(t,.0006,.012)
    x+=.25*modes(t,seed+1,430 if guard else 970,.045 if guard else .10)
    x+=.24*np.sin(2*np.pi*145*t)*envelope(t,.001,.048)
    if just:
        x+=.18*np.sin(2*np.pi*2350*t)*envelope(t,.001,.14)
        x+=.11*np.sin(2*np.pi*3520*t)*envelope(t,.002,.11)
    return finish(x,-9 if guard else -4 if just else -5.5)


def ready(seed, flux=False):
    t=time(.32)
    x=.16*modes(t,seed,880 if flux else 740,.065,5)
    for f in ((1320,1980) if flux else (1480,2220)):
        add(x,.14*np.sin(2*np.pi*f*t)*envelope(t,.003,.075),.045)
    return finish(x,-9)


def charge_loop():
    t=time(1)
    # 全成分を整数Hzにして周期と傾きを連続にする。
    x=sum(amp*np.sin(2*np.pi*f*t) for f,amp in ((110,.30),(221,.15),(337,.09),(557,.05)))
    x *= .8 + .2*np.cos(2*np.pi*24*t)
    return finish(x,-16,loop=True)


def build():
    jobs={}
    for kind in ('Down','Up','Spin','Slide','Air','Finish','Charge'):
        for v in (1,2): jobs[f'SE_GS_Swing_{kind}_{v:02}.wav']=swing(kind,v)
    for v in (1,2,3): jobs[f'SE_GS_Hit_{v:02}.wav']=impact(v)
    for v in (1,2):
        jobs[f'SE_GS_Hit_Heavy_{v:02}.wav']=impact(v,heavy=True)
        jobs[f'SE_GS_Execute_{v:02}.wav']=impact(v,heavy=True,execute=True)
        jobs[f'SE_GS_Parry_{v:02}.wav']=parry(v)
        jobs[f'SE_GS_ParryJust_{v:02}.wav']=parry(v,just=True)
        jobs[f'SE_GS_Guard_{v:02}.wav']=parry(v,guard=True)
        t=time(.14)
        jobs[f'SE_GS_Dash_{v:02}.wav']=finish(noise(.14,6500+v,120,2200)*envelope(t,.003,.035),-13)
    t=time(.12)
    jobs['SE_GS_Ready.wav']=finish(.15*modes(t,6600,520,.025)+.08*noise(.12,6601,900,4300)*envelope(t,.001,.015),-14)
    jobs['SE_GS_Charge_Loop.wav']=charge_loop()
    jobs['SE_GS_Charge_Full.wav']=ready(6700)
    jobs['SE_GS_Flux.wav']=ready(6800,flux=True)
    t=time(.18)
    jobs['SE_GS_Cadence.wav']=finish((np.sin(2*np.pi*880*t)+.23*np.sin(2*np.pi*1760*t))*envelope(t,.002,.040),-18)
    for v in (1,2):
        t=time(.22)
        jobs[f'SE_GS_Core_{v:02}.wav']=finish(modes(t,6900+v,1350,.06,6),-18)
    return jobs


def write(path,x,seed=77,loop=None):
    path.parent.mkdir(parents=True,exist_ok=True)
    if loop is None: loop = 'Loop' in path.name
    # TPDFディザ。ループは周期を優先して無ディザで量子化する。
    rng=np.random.default_rng(seed)
    dither=0 if loop else (rng.random(len(x))-rng.random(len(x)))/32767
    pcm=np.rint((x+dither)*32767).astype('<i2')
    if not loop: pcm[0]=pcm[-1]=0
    with wave.open(str(path),'wb') as w:
        w.setparams((1,2,SR,0,'NONE','not compressed'))
        w.writeframes(pcm.tobytes())
    return pcm.astype(float)/32768


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--preview',type=Path)
    args=parser.parse_args()
    jobs=build()
    metrics=[]
    for name,x in jobs.items():
        p=args.output/name
        y=write(p,x)
        meta=p.with_suffix('.wav.meta')
        if not meta.exists():
            meta.write_text("[meta]\nguid = '"+uuid.uuid4().hex+"'\n",encoding='utf-8')
        rms=np.sqrt(np.mean(y*y)); peak=np.max(np.abs(y))
        assert peak < .9 and abs(y.mean()) < .003
        metrics.append(dict(file=name,seconds=len(y)/SR,peak_db=round(float(20*np.log10(peak)),2),crest_db=round(float(20*np.log10(peak/rms)),2)))
    if args.preview:
        demo=np.zeros(SR*18)
        for i,kind in enumerate(('Down','Up','Down','Spin','Slide','Finish')):
            at=.35+i*.7
            add(demo,jobs[f'SE_GS_Swing_{kind}_01.wav'],at-.06)
            add(demo,jobs['SE_GS_Hit_Heavy_01.wav' if kind=='Finish' else f'SE_GS_Hit_{i%3+1:02}.wav'],at)
            if i: add(demo,jobs['SE_GS_Cadence.wav']*.5,at-.22)
        add(demo,jobs['SE_GS_Guard_01.wav'],5.5)
        add(demo,jobs['SE_GS_Parry_01.wav'],6.5)
        add(demo,jobs['SE_GS_ParryJust_01.wav'],7.5)
        add(demo,jobs['SE_GS_Charge_Loop.wav'],9)
        add(demo,jobs['SE_GS_Charge_Full.wav'],10)
        add(demo,jobs['SE_GS_Swing_Charge_01.wav'],10.65)
        add(demo,jobs['SE_GS_Hit_Heavy_02.wav'],10.71)
        add(demo,jobs['SE_GS_Ready.wav'],12.5)
        add(demo,jobs['SE_GS_Swing_Slide_01.wav'],13)
        add(demo,jobs['SE_GS_Execute_01.wav'],13.06)
        add(demo,jobs['SE_GS_Flux.wav'],15)
        add(demo,jobs['SE_GS_Hit_01.wav'],16.3)
        add(demo,jobs['SE_GS_Core_01.wav'],16.3)
        write(args.preview,finish(demo,-2))
    print(json.dumps(metrics,ensure_ascii=False,indent=2))


if __name__=='__main__':
    main()
