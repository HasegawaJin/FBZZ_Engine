"""新しいテクスチャ付き Player と候補骨格の位置を描画して確認する。"""
import bpy,json,numpy as np
from pathlib import Path
from mathutils import Vector
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'Docs/Art/MiniBotC/AnimationReady';OUT.mkdir(parents=True,exist_ok=True)
scene=bpy.context.scene
for o in scene.objects:
    if o.type=='MESH':o.hide_render=o.name!='Player_LOD0'
for i in range(3):bpy.data.collections['TRIPO_LOD'+str(i)].hide_render=i!=0
scene.render.engine='CYCLES';scene.cycles.samples=16;scene.cycles.device='GPU'
prefs=bpy.context.preferences.addons['cycles'].preferences;prefs.compute_device_type='OPTIX';prefs.refresh_devices()
for d in prefs.devices:d.use=d.type=='OPTIX'
cam=scene.camera;cam.data.type='ORTHO'
def render(name,location,target,scale,w=1200,h=1200):
    cam.location=location;cam.rotation_euler=(Vector(target)-cam.location).to_track_quat('-Z','Y').to_euler();cam.data.ortho_scale=scale
    scene.render.resolution_x=w;scene.render.resolution_y=h;scene.render.resolution_percentage=100
    scene.render.filepath=str(OUT/(name+'.png'));bpy.ops.render.render(write_still=True)
render('Hand_Outer',(3,.03,.70),(.38,.03,.70),.26)
render('Hand_Front',(.38,-3,.70),(.38,.03,.70),.26)
render('Hand_Palm',(-3,.02,.70),(.38,.02,.70),.26)
mat=bpy.data.materials.new('Inspection_Cyan');mat.use_nodes=True;nt=mat.node_tree;nt.nodes.clear()
em=nt.nodes.new('ShaderNodeEmission');em.inputs[0].default_value=(0,.6,1,1);em.inputs[1].default_value=1
out=nt.nodes.new('ShaderNodeOutputMaterial');nt.links.new(em.outputs[0],out.inputs[0])
for b in bpy.data.objects['MiniBotC_Humanoid'].data.bones:
    if b.name=='Root' or 'Hand' in b.name or 'Socket' in b.name:continue
    c=bpy.data.curves.new(b.name,'CURVE');c.dimensions='3D';c.bevel_depth=.002;c.resolution_u=1;c.bevel_resolution=1
    s=c.splines.new('POLY');s.points.add(1)
    for p,v in zip(s.points,[b.head_local,b.tail_local]):p.co=(v.x,-.40,v.z,1)
    obj=bpy.data.objects.new(b.name,c);scene.collection.objects.link(obj);c.materials.append(mat)
render('RigFit_Front',(0,-5,.86),(0,0,.86),1.90)
obj=bpy.data.objects['Player_LOD0'];v=np.array([v.co[:] for v in obj.data.vertices]);x,y,z=v.T
sections={}
for height in [.13,.54,.79,.98,1.155]:
    threshold=.28 if height>.7 and height<1.02 else .17 if height>1.02 else .08
    p=v[(abs(z-height)<.012)&(x>threshold)]
    sections[str(height)]={'min':p.min(axis=0).tolist(),'max':p.max(axis=0).tolist(),'mean':p.mean(axis=0).tolist()}
parent=np.arange(len(v))
def find(x):
    while parent[x]!=x:parent[x]=parent[parent[x]];x=parent[x]
    return x
for e in obj.data.edges:
    a,b=map(find,e.vertices);parent[b]=a
labels=np.array([find(i) for i in range(len(v))]);ids,counts=np.unique(labels,return_counts=True)
components=[]
for idx in np.argsort(counts)[::-1][:35]:
    p=v[labels==ids[idx]];components.append({'n':int(counts[idx]),'min':p.min(axis=0).tolist(),'max':p.max(axis=0).tolist()})
(OUT/'RigFit.json').write_text(json.dumps({'sections':sections,'components':components},indent=2),encoding='utf8')
print('RIG FIT INSPECTION COMPLETE',flush=True)
