"""配置済みPlayerだけを更新し、原本アセットとGUID参照を接続する。"""
import copy
import hashlib
import json
import re
import struct
import tomllib
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / 'GreenWare'
OUT = ROOT / 'Temp/PlayerScenePlacement'
SCENE = PROJECT / 'Assets/Scenes/Stage_01.scene'
MANIFEST = json.loads((PROJECT/'Assets/Models/Player/ExportManifest.json').read_text(encoding='utf-8'))
CLIPS = {c['take']:c for c in MANIFEST['clips']}


def read(path):
    return tomllib.loads(path.read_text(encoding='utf-8-sig'))


def value(v):
    if isinstance(v, bool): return str(v).lower()
    if isinstance(v, str): return json.dumps(v, ensure_ascii=False)
    if isinstance(v, list): return '[ '+', '.join(value(i) for i in v)+' ]'
    return str(v)


def table(data, prefix='', header=False, array=False):
    lines = [('[[%s]]' if array else '[%s]') % prefix] if header else []
    for k,v in data.items():
        if not isinstance(v, dict) and not (isinstance(v,list) and v and isinstance(v[0],dict)):
            lines.append(k+' = '+value(v))
    for k,v in data.items():
        key=prefix+'.'+k if prefix else k
        if isinstance(v,dict): lines += ['',table(v,key,True)]
        elif isinstance(v,list) and v and isinstance(v[0],dict):
            for item in v: lines += ['',table(item,key,True,True)]
    return '\n'.join(lines)+'\n'


def ref(path):
    meta=Path(str(path)+'.meta')
    if not meta.exists(): meta.write_text('[meta]\nguid = '+value(uuid.uuid4().hex)+'\n',encoding='utf-8')
    return 'guid:'+read(meta)['meta']['guid']+'|'+path.relative_to(PROJECT).as_posix()


def material(name, source, textures):
    path=PROJECT/'Assets/Materials/Player'/name
    data=read(source)
    data['name']=path.stem
    data['textures']={slot:ref(PROJECT/'Assets/Models/Player/Textures'/tex) for slot,tex in textures.items()}
    path.write_text(table(data),encoding='utf-8')
    return ref(path)


def make_object(name,parent=None,position=None,rotation=None):
    return dict(active=True,instanceId=str(uuid.uuid4()),layer=0,name=name,parent=parent['name'] if parent else '',
                parentInstanceId=parent['instanceId'] if parent else '',prefabAssetPath='',prefabSourceId='',tag='Untagged',
                transform=dict(position=position or [0.0,0.0,0.0],rotation=rotation or [0.0,0.0,0.0,1.0],scale=[1.0,1.0,1.0]))


def main():
    text=SCENE.read_text(encoding='utf-8-sig')
    before=tomllib.loads(text)
    player=next(o for o in before['gameobjects'] if o['name']=='Player')
    assert player['instanceId']=='4c0fca36-3be7-4e7b-b273-6a32a310a878'
    assert not player.get('ScriptComponents'), 'Already configured; do not duplicate'
    snapshot=OUT/'BeforeComponents/Stage_01_BeforeConnect.scene'
    assert not snapshot.exists()
    snapshot.write_text(text,encoding='utf-8')
    saved=read(ROOT/'Docs/Art/MiniBotC/ExportDelivery/SceneBackups/Stage_01.scene')
    old=next(o for o in saved['gameobjects'] if o['name']=='Player')
    controller=PROJECT/'Assets/Animation/Player/Player.animcontroller'
    ct=controller.read_text(encoding='utf-8')
    mask=read(controller)['baseLayerMaskPath']
    assert '\nlayers = []\n' in ct
    ct=ct.replace('\nlayers = []\n','\n',1)
    for name,weight in [('Attack',0.0),('HitReaction',1.0)]:
        ct+='\n'+table(dict(name=name,mode=0,weight=weight,enabled=True,maskPath=mask,states=[],anyStateTransitions=[],defaultStateName=''), 'layers',True,True)
    tomllib.loads(ct)
    controller.write_text(ct,encoding='utf-8')

    player['tag']='Player'
    player['AnimatorComponent']=dict(enabled=True,controllerPath=ref(controller),defaultStateName='Locomotion',playing=True,speed=1.0,
        externalPose=False,rootMotionMode=0,rootMotionPoseMode=0,rootMotionApplyXZ=0,rootMotionApplyY=0,rootMotionApplyRotation=0)
    for component in ('RigidBodyComponent','CharacterControllerComponent','CapsuleColliderComponent','AudioSourceComponent'):
        player[component]=copy.deepcopy(old[component])
    player['RigidBodyComponent'].update(useCCD=True,ccdRadius=0.3)
    player['CapsuleColliderComponent']['center']=[0.0,0.82,0.0]
    player['CapsuleColliderComponent']['shape'].update(radius=0.32,halfHeight=0.5)
    player['CapsuleColliderComponent']['material']['restitution']=0.0
    # Actorの位置は保持し、DCCの前方-Zをゲームの前方+Zへ合わせる。
    player['transform']['rotation']=[0.0,1.0,0.0,0.0]

    model_guid=read(PROJECT/'Assets/Models/Player/Player.fbx.meta')['meta']['guid']
    sword_guid=read(PROJECT/'Assets/Models/Player/Sword/Sword.fbx.meta')['meta']['guid']
    materials=PROJECT/'Library/Baked'/model_guid/'materials'
    body=material('Player_Body.mat',materials/'PlayerExport_Player_LOD0_PBR.mat',{'albedo':'Player_BaseColor.jpg','normal':'Player_LOD0_Normal.png'})
    fingers=material('Player_Fingers.mat',materials/'PlayerExport_Player_Fingers_LOD0_Tripo.mat',{'albedo':'Player_Fingers_LOD0_BaseColor.png'})
    sword_mat=material('Player_Sword.mat',PROJECT/'Library/Baked'/sword_guid/'materials/PlayerExport_Sword_LOD0_PBR.mat',{'albedo':'Sword_LOD0_BaseColor.png','normal':'Sword_LOD0_Normal.png'})
    for name in ('Player_LOD0_Normal.png','Sword_LOD0_Normal.png'):
        path=PROJECT/'Assets/Models/Player/Textures'/(name+'.meta')
        metadata=read(path)
        metadata['texture']=dict(type='normal',srgb=False,flip_green=True,mipmaps=True,normalize_mipmaps=True)
        path.write_text(table(metadata),encoding='utf-8')

    sword=make_object('Sword',player)
    sword['MeshRenderer']=dict(enabled=True,meshPath='guid:'+sword_guid+':0|Assets/Models/Player/Sword/Sword.fbx',castShadows=True)
    sword['MaterialComponent']=dict(enabled=True,material=sword_mat,visible=True)
    sword['SocketAttachmentComponent']=dict(enabled=True,target=player['instanceId'],socketName='Socket_Weapon_R',localSocketName='Attach_Grip',
        followPosition=True,followRotation=True,followScale=False,blendDuration=0.0,positionOffset=[0.0,0.0,0.0],rotationOffsetDegrees=[0.0,0.0,0.0],scaleMultiplier=[1.0,1.0,1.0])
    # FBXインポーターが持つAttach_Gripの座標系をそのまま保持する。
    grip=make_object('Attach_Grip',sword,rotation=[0.7071067812,0.0,0.0,0.7071067812])
    tips=[make_object(name,sword,position=[0.0,y,0.0]) for name,y in [('SOCKET_Tip',1.16),('SOCKET_Trail_Base',0.16),('SOCKET_Trail_Tip',1.16)]]

    f=dict(tuning=ref(PROJECT/'Assets/Data/PlayerTuning.fzdata'),bladeTuning=ref(PROJECT/'Assets/Data/BladeTuning.fzdata'),
        sword=sword['instanceId'],gripSocketName='Attach_Grip',useFootIK=False,modelYawOffsetDegrees=180.0,
        footBoneLeftName='LeftFoot',footBoneRightName='RightFoot',headBoneName='Head',spineBoneName='Spine',chestBoneName='Chest',
        headWeight=0.0,lookWhenHolstered=False,locomotionStateName='Locomotion',dodgeStateName='Dodge',landStateName='Land',
        runClipSeconds=0.6,dodgeClipSeconds=1.4,dodgeAnimFollow=0.0,landFastSpeed=1.0,
        slashLayerName='Attack',airSlashLayerName='Attack',hitLayerName='HitReaction',slashFadeIn=0.06,slashFadeOut=0.12,
        slashWindup=1.0,slashFinisherWindup=1.0,slashChargedWindup=1.0,slashFollowThrough=1.0,
        layerName='',parryClipFile='',parryClipName='',executeClipFile='',executeClipName='',
        trailOwnerA='Sword',trailOwnerB='',dodgeCancelAt=1.0)
    maps={'slashRight':'Slash01','doubleRight':'UnderSlashandUpperSlash','return':'UnderSlash','spin':'HighSpinAttack',
          'rise':'SlideAttack','finisherRight':'JumpAttack','charged':'HighSpinAttack','chargeHold':'Blocking',
          'chargeRelease':'HighSpinAttack','airSlash':'JumpAttack','jumpSlam':'JumpAttack','hitFront':'Hit'}
    for prefix,take in maps.items():
        f[prefix+'ClipFile']=CLIPS[take]['runtime_ref']; f[prefix+'ClipName']=take
    for field,num in {'slashHitTime':24,'doubleHitTime':20,'returnHitTime':26,'spinHitTime':34,'riseHitTime':39,
                      'finisherHitTime':34,'chargedHitTime':34,'chargeReleaseHitTime':34,'airSlashHitTime':34,'jumpSlamHitTime':34}.items():
        f[field]=num/30.0
    player['ScriptComponents']=[dict(type='PlayerComponent',enabled=True,fields=f)]
    for script in old['ScriptComponents']:
        if script['type'] in ('PlayerHealthBarComponent','SeWarmupComponent','PlayerBossBlockComponent'):
            player['ScriptComponents'].append(copy.deepcopy(script))

    changed={player['instanceId']:player}
    for obj in before['gameobjects']:
        if obj.get('parentInstanceId')==player['instanceId'] and 'SkinnedMeshRenderer' in obj:
            obj['MaterialComponent']=dict(enabled=True,visible=True,material=fingers if obj['SkinnedMeshRenderer']['submeshIndices']==[0] else body)
            changed[obj['instanceId']]=obj
    chunks=re.split(r'(?m)^\[\[gameobjects\]\]\s*\n',text)
    for i in range(1,len(chunks)):
        instance=re.search(r'(?m)^instanceId\s*=\s*[\'\"]([^\'\"]+)',chunks[i]).group(1)
        if instance in changed: chunks[i]=table(changed[instance],'gameobjects').rstrip()+'\n\n'
    result=chunks[0]+''.join('[[gameobjects]]\n'+c for c in chunks[1:])
    for obj in [sword,grip,*tips]: result+='\n'+table(obj,'gameobjects',True,True)
    after=tomllib.loads(result)
    ids=[o['instanceId'] for o in after['gameobjects']]
    assert len(ids)==len(set(ids))
    for o in after['gameobjects']:
        assert not o.get('parentInstanceId') or o['parentInstanceId'] in ids
    original=tomllib.loads(text)
    unmodified={o['instanceId']:o for o in original['gameobjects'] if o['instanceId'] not in changed}
    assert all(o==unmodified[o['instanceId']] for o in after['gameobjects'] if o['instanceId'] in unmodified)
    assert SCENE.read_text(encoding='utf-8-sig')==text, 'Scene changed during patch preparation'
    result=result.rstrip()+'\n'
    SCENE.write_text(result,encoding='utf-8')
    report=dict(scene=str(SCENE),player=player['instanceId'],position=player['transform']['position'],sword=sword['instanceId'],
                clips=len(CLIPS),existing_objects_preserved=len(unmodified),added_objects=5,changed_objects=len(changed),components=list(player),
                before_sha256=hashlib.sha256(text.encode()).hexdigest(),after_sha256=hashlib.sha256(result.encode()).hexdigest())
    (OUT/'ConnectionReport.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))


if __name__=='__main__': main()
