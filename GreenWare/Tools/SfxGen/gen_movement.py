"""PlayerとBossの移動音。48kHz/16bit PCM mono、録音素材不使用。

--output GreenWare/Assets/Sound/SE --preview <directory>
既存WAVは内容のみ更新しmetaを保持する。音源の正本は本生成器。
"""
from pathlib import Path
import argparse
import json
import uuid
import numpy as np
from gen_greatsword import SR, time, noise, modes, envelope, finish, write, add, charge_loop


def motor(seconds, seed, heavy=False):
    t=time(seconds)
    base=55 if heavy else 240
    # 有限区間のDFT基底へ揃え、振幅変調も周期を閉じる。
    hz=lambda f: round(f*seconds)/seconds
    x=sum(a*np.sin(2*np.pi*hz(base*r)*t+.3*i)
          for i,(r,a) in enumerate(((1,.3),(2.03,.16),(3.81,.08),(6.4,.035))))
    x*=.8+.2*np.cos(2*np.pi*hz(7 if heavy else 19)*t)
    x+=noise(seconds,seed,70 if heavy else 600,650 if heavy else 2400)*.04
    return x


def actuator(seconds, seed, loaded=False, braking=False):
    t=time(seconds)
    u=t/seconds
    # チャージの倍音と脈動を高域へ移し、短い加速・制動で機敏な駆動にする。
    source=charge_loop()
    sweep=np.exp(-t/.035) if braking else 1-np.exp(-t/.010)
    speed=((3.4 if loaded else 4.2)+.65*sweep)*(1+(seed%7-3)*.006)
    position=(np.cumsum(speed)-speed[0])%len(source)
    x=np.interp(position,np.arange(len(source)+1),np.append(source,source[0]))
    attack=1-np.exp(-t/.003)
    release=np.sin(np.minimum(1,(1-u)/.32)*np.pi/2)**2
    return x*attack*release


def foot(variant, running=False, boss=False):
    if not boss:
        duration=.16 if running else .20
        return finish(actuator(duration,8100+variant*37+int(running)*500),
                      (-18 if running else -20)+(variant%3-1)*.35)
    duration=.36 if boss else .18 if running else .22
    t=time(duration); seed=8100+variant*37+int(running)*500+int(boss)*1000
    base=(135 if boss else 570)*(1+(variant%4-1.5)*.045)
    x=.18*modes(t,seed,base,.075 if boss else .021,8)
    x+=.18*noise(duration,seed+1,160 if boss else 950,2600 if boss else 5100)*envelope(t,.0008,.012)
    # 足裏の二次接触は歩ごとに時差と強さを変える。
    sole=.12*modes(t,seed+2,base*.72,.05 if boss else .025,6)
    add(x,sole,(.026 if running else .042)+.003*(variant%3))
    hz=58 if boss else 145
    x+=(.55 if boss else .13)*np.sin(2*np.pi*(hz*t+1.4*(1-np.exp(-t/.018))))*envelope(t,.002,.10 if boss else .034)
    if variant%3!=0:
        x+=.08*noise(duration,seed+3,330,2100)*envelope(t,.012,.055 if boss else .025)
    if boss:
        x+=motor(duration,seed+4,True)*envelope(t,.004,.07)*.09
    else:
        x+=actuator(duration,seed+4)*.65
    return finish(x,(-8 if boss else -12.5 if running else -14)+(variant%3-1)*.65)


def motion(kind, variant=1):
    duration=dict(Dodge=.22,DodgeEnd=.14,Landing=.22,Servo=.18,Climb=.23,Jump=.20,Stop=.18)[kind]
    t=time(duration); seed=10400+list(('Dodge','DodgeEnd','Landing','Servo','Climb','Jump','Stop')).index(kind)*70+variant
    x=actuator(duration,seed,loaded=kind in ('Landing','Climb'),
               braking=kind in ('Landing','Stop','DodgeEnd'))
    db=dict(Dodge=-16,DodgeEnd=-22,Landing=-20,Servo=-23,Climb=-21,Jump=-18,Stop=-22)[kind]
    return finish(x,db+(variant%3-1)*.45)


def cycle(seconds,variant,loop=False):
    x=np.zeros(round(seconds*SR))
    for step in range(4):
        y=foot(variant*4+step,True,True)
        offset=round(step*seconds*SR/4)
        # 足の余韻は周期境界へ折り返す。ループ境界で消音しない。
        np.add.at(x,(np.arange(len(y))+offset)%len(x),y)
    return finish(x,-9 if loop else -8,loop=loop)


def serpent(seconds,seed,fast=False):
    t=time(seconds)
    rough=noise(seconds,seed,180,1700 if fast else 1150)
    rate=(round((9 if fast else 4)*seconds)/seconds)
    x=.19*rough*(.45+.55*np.sin(2*np.pi*rate*t)**4)
    x+=motor(seconds,seed+1,True)*.14
    x+=noise(seconds,seed+2,1900,4700)*.025
    return finish(x,-14 if fast else -19,loop=True)


def boss_event(kind,variant=1):
    duration=dict(Windup=1.5,Crash=1.15,Recover=1.4,Appear=5.0,Landing=.8,Rear=.62)[kind]
    t=time(duration); seed=12300+list(('Windup','Crash','Recover','Appear','Landing','Rear')).index(kind)*80+variant
    x=np.zeros(len(t))
    if kind in ('Windup','Recover','Rear'):
        e=np.sin(np.pi*t/duration)**1.4
        x+=motor(duration,seed,True)*e*.35
        x+=noise(duration,seed+1,250,2300)*e*.08
        for at in (.03,duration*.67): add(x,foot(variant,False,True)*.2,at)
    elif kind=='Appear':
        x+=noise(duration,seed,80,1100)*np.exp(-.5*((t-1.4)/.65)**2)*.12
        for i in range(4): add(x,foot(i,False,True),2.0+i*.065)
        add(x,motor(2,seed+1,True)*envelope(time(2),.04,.4)*.3,2.1)
    else:
        for i,at in enumerate((0,.027,.064)):
            add(x,foot(variant+i,True,True)*(1-.17*i),at)
        x+=.32*np.sin(2*np.pi*47*t)*envelope(t,.003,.14)
        x+=.10*noise(duration,seed,220,3300)*envelope(t,.005,.11)
    return finish(x,dict(Windup=-15,Crash=-5,Recover=-16,Appear=-8,Landing=-8,Rear=-17)[kind])


def build():
    jobs={}
    source=charge_loop()
    jobs['Player/SE_PL_Motor_Loop.wav']=finish(source[(np.arange(SR)*4)%SR],-18,loop=True)
    for kind,prefix,count in [('Dodge','Dodge',3),('Landing','Landing',2),('Servo','Servo_Turn',3),('Climb','Climb_Grab',3),('Jump','Jump',2)]:
        for v in range(1,count+1): jobs[f'Player/SE_PL_{prefix}_{v:02}.wav']=motion(kind,v)
    jobs['Player/SE_PL_Dodge_End.wav']=motion('DodgeEnd')
    for kind,seconds in [('Walk',40/30),('Charge',1.0)]:
        for v in range(1,5): jobs[f'Boss/SE_BOSS_Step_{kind}_{v:02}.wav']=cycle(seconds,v)
    jobs['Boss/SE_BOSS_Servo_Loop.wav']=finish(motor(2,14000,True),-23,loop=True)
    jobs['Boss/SE_BOSS_Charge_Run.wav']=cycle(2/3,5,loop=True)
    jobs['Boss/SE_BOSS_Stun_Loop.wav']=finish(motor(2,14001,True)*(.65+.35*np.cos(2*np.pi*2*time(2))),-25,loop=True)
    for file,kind in [('Charge_Windup','Windup'),('Charge_Crash','Crash'),('Stun_Recover','Recover'),('Appear','Appear')]:
        jobs[f'Boss/SE_BOSS_{file}.wav']=boss_event(kind)
    for v in (1,2):
        jobs[f'Boss/SE_BOSS_Landing_{v:02}.wav']=boss_event('Landing',v)
        jobs[f'Enemy/SE_SERP_Rear_{v:02}.wav']=boss_event('Rear',v)
    jobs['Enemy/SE_SERP_Crawl_Loop.wav']=serpent(2,15000)
    jobs['Enemy/SE_SERP_Rush_Loop.wav']=serpent(1,15001,True)
    return jobs


def demo(jobs,folder):
    player=np.zeros(15*SR)
    source=jobs['Player/SE_PL_Motor_Loop.wav']
    gain=0.; pitch=.85; position=0.
    for frame in range(15*60):
        at=frame/60
        ratio=.35 if .2<=at<5 else 1. if 6<=at<9.65 or 12<=at<12.3 else 0.
        target=.35+.4*ratio if ratio else 0.
        gain+=np.clip(target-gain,-1/(60*.30),1/(60*.08))
        pitch+=((.95+.4*ratio if ratio else .85)-pitch)*(1-np.exp(-1/(60*.10)))
        indices=(position+np.arange(SR//60)*pitch)%len(source)
        player[frame*(SR//60):(frame+1)*(SR//60)]=np.interp(indices,np.arange(len(source)+1),np.append(source,source[0]))*gain
        position=(position+SR/60*pitch)%len(source) if gain>.001 else 0.
    for key,at in [('Jump_01',10.35),('Landing_01',11.2),('Dodge_01',12),('Dodge_End',12.36),('Climb_Grab_01',13),('Climb_Grab_02',13.5),('Servo_Turn_01',14.2)]:
        add(player,jobs[f'Player/SE_PL_{key}.wav'],at)
    boss=np.zeros(15*SR)
    for i in range(3): add(boss,jobs[f'Boss/SE_BOSS_Step_Walk_{i+1:02}.wav'],.2+i*40/30)
    add(boss,jobs['Boss/SE_BOSS_Charge_Windup.wav'],4.4)
    for i in range(5): add(boss,jobs['Boss/SE_BOSS_Charge_Run.wav'],6+i*2/3)
    add(boss,jobs['Boss/SE_BOSS_Charge_Crash.wav'],9.4)
    add(boss,jobs['Boss/SE_BOSS_Stun_Loop.wav'],10.6)
    add(boss,jobs['Boss/SE_BOSS_Stun_Recover.wav'],12.8)
    snake=np.zeros(9*SR)
    for i in range(2): add(snake,jobs['Enemy/SE_SERP_Crawl_Loop.wav'],i*2)
    add(snake,jobs['Enemy/SE_SERP_Rear_01.wav'],4.2)
    for i in range(3): add(snake,jobs['Enemy/SE_SERP_Rush_Loop.wav'],5+i)
    # デモ同士もゲーム内と同じ相対音量で比較する。個別にピークを揃えない。
    for name,x in [('Player_Movement',player),('Boss_Movement',boss),('Serpent_Movement',snake)]:
        assert abs(x).max()<.95
        write(folder/(name+'.wav'),x)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--preview',type=Path)
    args=parser.parse_args()
    jobs=build()
    for name,x in jobs.items():
        p=args.output/name
        write(p,x,loop=('Loop' in name or name.endswith('Charge_Run.wav')))
        meta=p.with_suffix('.wav.meta')
        if not meta.exists(): meta.write_text("[meta]\nguid = '"+uuid.uuid4().hex+"'\n",encoding='utf-8')
    if args.preview: demo(jobs,args.preview)
    print(json.dumps(dict(files=len(jobs),seconds=round(sum(len(x)/SR for x in jobs.values()),2))))


if __name__=='__main__': main()
