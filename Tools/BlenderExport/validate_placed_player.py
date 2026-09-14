"""配置済みPlayerの参照・スケルトン・クリップ・剣の合わせ点を検証する。"""
import json
import math
import struct
import tomllib
from pathlib import Path
import numpy as np

ROOT=Path(__file__).resolve().parents[2]
PROJECT=ROOT/'GreenWare'
OUT=ROOT/'Temp/PlayerScenePlacement'


def read(path): return tomllib.loads(path.read_text(encoding='utf-8-sig'))


def matrix(p,q,s):
    x,y,z,w=np.array(q)/np.linalg.norm(q)
    r=np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
    m=np.eye(4);m[:3,:3]=r@np.diag(s);m[:3,3]=p
    return m


def skeleton(path):
    b=path.read_bytes();assert b[:4]==b'FZSK'
    n=struct.unpack_from('<I',b,12)[0];o=84;nodes=[]
    for i in range(n):
        name=b[o:o+64].split(b'\0')[0].decode()
        parent,bone=struct.unpack_from('<ii',b,o+64)
        trs=struct.unpack_from('<10f',b,o+72)
        children=struct.unpack_from('<I',b,o+176)[0]
        nodes.append(dict(name=name,parent=parent,bone=bone,p=trs[:3],q=trs[3:7],s=trs[7:]))
        o+=180+children*4
    return nodes


def clip(path):
    b=path.read_bytes();assert b[:4]==b'FZAN'
    version=struct.unpack_from('<I',b,4)[0]
    duration,tps,count=struct.unpack_from('<ddI',b,136)
    o=160+(48 if version>=2 else 0);tracks={}
    for _ in range(count):
        name=b[o+256:o+384].split(b'\0')[0].decode()
        counts=struct.unpack_from('<III',b,o+388);o+=400;keys=[]
        for n in counts:
            arr=np.array([struct.unpack_from('<d4f',b,o+24*j) for j in range(n)])
            keys.append(arr);o+=24*n
        tracks[name]=keys
    return duration,tps,tracks


def sample(arr,t,rotation=False):
    if len(arr)==1:return arr[0,1:5 if rotation else 4]
    i=int(np.searchsorted(arr[:,0],t,side='right'))
    if i==0:return arr[0,1:5 if rotation else 4]
    if i==len(arr):return arr[-1,1:5 if rotation else 4]
    a,b=arr[i-1],arr[i];weight=(t-a[0])/(b[0]-a[0])
    va,vb=a[1:5 if rotation else 4],b[1:5 if rotation else 4]
    if rotation and np.dot(va,vb)<0:vb=-vb
    result=va*(1-weight)+vb*weight
    return result/np.linalg.norm(result) if rotation else result


def main():
    scene=read(PROJECT/'Assets/Scenes/Stage_01.scene')
    objects=scene['gameobjects'];by_id={o['instanceId']:o for o in objects}
    player=next(o for o in objects if o['name']=='Player')
    sword=next(o for o in objects if o['name']=='Sword')
    assert len(by_id)==len(objects)
    assert player['tag']=='Player' and 'RigidBodyComponent' in player and 'CapsuleColliderComponent' in player
    assert [s['type'] for s in player['ScriptComponents']].count('PlayerComponent')==1
    assert not any(o['name'] in ('WPN_Sword_L','WPN_Sword_R') for o in objects)
    assert not any(s['type']=='PlayerClimbComponent' for s in player['ScriptComponents'])
    refs=[]
    def walk(v):
        if isinstance(v,dict):
            for x in v.values():walk(x)
        elif isinstance(v,list):
            for x in v:walk(x)
        elif isinstance(v,str) and v.startswith('guid:'):
            guid,path=v[5:].split('|',1);path=PROJECT/path
            assert path.exists(),str(path)
            meta=Path(str(path)+'.meta')
            if meta.exists():assert read(meta)['meta']['guid']==guid.split(':')[0],str(meta)
            refs.append(v)
    walk(player);walk(sword)
    for o in objects:
        if o.get('parentInstanceId')==player['instanceId'] and 'SkinnedMeshRenderer' in o:
            assert o['SkinnedMeshRenderer']['skeletonRootGuid'] in by_id
            walk(o)
            material=PROJECT/o['MaterialComponent']['material'].split('|',1)[1]
            walk(read(material))
    walk(read(PROJECT/sword['MaterialComponent']['material'].split('|',1)[1]))
    controller=read(PROJECT/'Assets/Animation/Player/Player.animcontroller')
    walk(controller)
    fields=next(s['fields'] for s in player['ScriptComponents'] if s['type']=='PlayerComponent')
    assert fields['sword']==sword['instanceId']
    assert fields['dodgeStateName'] in {s['name'] for s in controller['states']}
    assert fields['landStateName'] in {s['name'] for s in controller['states']}
    assert {fields['slashLayerName'],fields['airSlashLayerName'],fields['hitLayerName']} <= {s['name'] for s in controller['layers']}
    assert all(s['mode']==0 and s['maskPath']==controller['baseLayerMaskPath'] for s in controller['layers'])
    model_guid=read(PROJECT/'Assets/Models/Player/Player.fbx.meta')['meta']['guid']
    nodes=skeleton(PROJECT/'Library/Baked'/model_guid/'Player.skel')
    bones={n['name']:i for i,n in enumerate(nodes)}
    socket=bones['Socket_Weapon_R'];hand=bones['RightHand']
    grip=next(o for o in objects if o['name']=='Attach_Grip' and o['parentInstanceId']==sword['instanceId'])['transform']
    grip_inverse=np.linalg.inv(matrix(grip['position'],grip['rotation'],grip['scale']))
    manifest=json.loads((PROJECT/'Assets/Models/Player/ExportManifest.json').read_text(encoding='utf-8'))
    samples=[]
    for c in manifest['clips']:
        duration,tps,tracks=clip(PROJECT/c['runtime_path'])
        assert abs(duration/tps-c['duration_seconds'])<1e-4
        assert 'Socket_Weapon_R' in tracks
        distances=[];ground=[]
        for t in np.linspace(0,duration,9):
            worlds=[]
            for node in nodes:
                p,q,s=node['p'],node['q'],node['s']
                if node['name'] in tracks:
                    a,b,d=tracks[node['name']]
                    if len(a):p=sample(a,t)
                    if len(b):q=sample(b,t,True)
                    if len(d):s=sample(d,t)
                m=matrix(p,q,s)
                if node['parent']>=0:m=worlds[node['parent']]@m
                worlds.append(m)
            weapon=worlds[socket]@grip_inverse
            assert np.all(np.isfinite(weapon))
            assert np.max(np.abs(weapon@np.linalg.inv(grip_inverse)-worlds[socket]))<1e-6
            distances.append(float(np.linalg.norm(worlds[socket][:3,3]-worlds[hand][:3,3])))
            ground.append(float((weapon@np.array([0,1.16,0,1]))[1]))
        samples.append(dict(clip=c['take'],socket_to_hand_range=[min(distances),max(distances)],blade_tip_height_range=[min(ground),max(ground)]))
    report=dict(object_count=len(objects),validated_guid_references=len(refs),skeleton_nodes=len(nodes),clips=len(samples),
                player_position=player['transform']['position'],player_id=player['instanceId'],sword_id=sword['instanceId'],clip_samples=samples)
    (OUT/'ConnectionValidation.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='clip_samples'}))
    print(json.dumps(samples))


if __name__=='__main__':main()
