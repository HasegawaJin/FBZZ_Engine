"""最適化済み Tripo の配色と形状を保ち、制作リグとゲーム骨格を組み込む。"""
import ast,json,math,time
from pathlib import Path
import bpy,numpy as np
from mathutils import Vector,Matrix

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'GreenWare/Assets/_src/MiniBotC/AnimationReady';OUT.mkdir(parents=True,exist_ok=True)
ART=ROOT/'Docs/Art/MiniBotC/AnimationReady';ART.mkdir(parents=True,exist_ok=True)
scene=bpy.context.scene;scene.name='MiniBotC_AnimationReady'
def activate(o):
    if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT');o.hide_set(False);o.select_set(True);bpy.context.view_layer.objects.active=o
def update():
    bpy.context.view_layer.update();scene.frame_set(scene.frame_current)
def coords(o):
    a=np.empty(len(o.data.vertices)*3,np.float32);o.data.vertices.foreach_get('co',a);return a.reshape(-1,3)
def bind(o,rig):
    world=o.matrix_world.copy();o.parent=rig;o.matrix_world=world
    for m in list(o.modifiers):
        if m.type=='ARMATURE':o.modifiers.remove(m)
    m=o.modifiers.new('Humanoid Skin - 4 weights','ARMATURE');m.object=rig;m.use_deform_preserve_volume=False
def write_weights(o,names,w):
    keep=np.argpartition(w,-4,axis=1)[:,-4:]
    result=np.zeros_like(w);np.put_along_axis(result,keep,np.take_along_axis(w,keep,axis=1),axis=1)
    result[result<.001]=0;result/=result.sum(axis=1,keepdims=True)
    integers=np.rint(result*4096).astype(np.int32)
    integers[np.arange(len(w)),integers.argmax(axis=1)]+=4096-integers.sum(axis=1)
    o.vertex_groups.clear()
    for i,name in enumerate(names):
        ids=np.flatnonzero(integers[:,i]>0)
        if not len(ids):continue
        g=o.vertex_groups.new(name=name)
        for value in np.unique(integers[ids,i]):g.add(ids[integers[ids,i]==value].tolist(),float(value)/4096,'REPLACE')
    return integers/4096

def split_finger_bridges(o):
    mesh=o.data;uv=[tuple(p.uv) for p in mesh.uv_layers.active.data]
    normals=[tuple(n.vector) for n in mesh.corner_normals]
    vs=[];faces=[];parts=[];remap={};labels=['Index','Middle','Ring','Pinky']
    center_y=[.012,.043,.074,.101]
    for p in mesh.polygons:
        center=sum((mesh.vertices[i].co for i in p.vertices),Vector())/len(p.vertices)
        a=abs(center.x);label=-1
        if .565<center.z<.744 and a>.285:
            if a<.367 and center.z<.735:label=4
            elif a>=.367 and center.z<.706:
                shift=max(0,.706-center.z)*.24
                label=int(np.argmin(np.abs(np.array(center_y)-shift-center.y)))
            if label>=0 and center.x<0:label+=5
        face=[]
        for index in p.vertices:
            key=(index,label)
            if key not in remap:
                remap[key]=len(vs);vs.append(mesh.vertices[index].co[:]);parts.append(label)
            face.append(remap[key])
        faces.append(face)
    new=bpy.data.meshes.new(mesh.name+'_Articulated');new.from_pydata(vs,[],faces)
    new.uv_layers.new(name='UVMap')
    for dst,src in zip(new.uv_layers.active.data,uv):dst.uv=src
    for dst,src in zip(new.polygons,mesh.polygons):dst.use_smooth=src.use_smooth;dst.material_index=src.material_index
    for mat in mesh.materials:new.materials.append(mat)
    new.normals_split_custom_set(normals)
    attr=new.attributes.new('rig_part','INT','POINT');attr.data.foreach_set('value',parts)
    o.data=new
    return {'vertices_before':len(mesh.vertices),'vertices_after':len(vs),'triangles':len(faces),'split_finger_vertices':sum(p>=0 for p in parts)}

# 前版で検証した胴体の領域分類を再使用し、指は今回の元メッシュへ新しく割り当てる。
old=ast.parse((ROOT/'Tools/BlenderExport/rig_tripo_minibot.py').read_text(encoding='utf8'))
functions=[n for n in old.body if isinstance(n,ast.FunctionDef) and n.name in ['Smooth','SkinBody']]
exec(compile(ast.Module(body=functions,type_ignores=[]),'rig_body_weights','exec'),globals())
oldrig=bpy.data.objects['MiniBotC_Humanoid'];mapping=json.loads(oldrig['humanoid_mapping'])
parents={b.name:b.parent.name if b.parent else None for b in oldrig.data.bones}
meta=bpy.data.objects['MiniBotC_Metarig']
oldfinger={b.name:(b.head_local.copy(),b.tail_local.copy()) for b in meta.data.bones if any(n in b.name for n in ['thumb','f_index','f_middle','f_ring','f_pinky'])}
for o in list(bpy.data.objects):
    if o.name in ['MiniBotC_ControlRig','MiniBotC_Humanoid','LeftHand_GripTarget'] or o.name.startswith('WGT-RIG-MiniBotC'):
        bpy.data.objects.remove(o,do_unlink=True)
for c in list(bpy.data.collections):
    if c.name.startswith('WGTS_') and not c.objects:bpy.data.collections.remove(c)
activate(meta);meta.animation_data_clear()
for p in meta.pose.bones:p.matrix_basis=Matrix.Identity(4)
bpy.ops.object.mode_set(mode='EDIT')
for b in meta.data.edit_bones:
    b.head.y+=.035;b.tail.y+=.035
for side,sign in [('L',1),('R',-1)]:
    b=meta.data.edit_bones['hand.'+side];b.head.y=.055;b.tail.y=.045
    meta.data.edit_bones['forearm.'+side].tail=b.head
    finger_levels={'f_index':[.706,.665,.629,.593],'f_middle':[.700,.651,.614,.578],
                   'f_ring':[.700,.656,.621,.584],'f_pinky':[.704,.668,.636,.608]}
    finger_y={'f_index':.012,'f_middle':.043,'f_ring':.074,'f_pinky':.101}
    for stem in ['f_index','f_middle','f_ring','f_pinky','thumb']:
        for j in range(1,4):
            b=meta.data.edit_bones[f'{stem}.{j:02d}.{side}'];a,t=oldfinger[b.name]
            if stem=='thumb':b.head=a+Vector((0,.012,0));b.tail=t+Vector((0,.012,0))
            else:
                zs=finger_levels[stem];xs=[.416,.429,.416,.398];ys=[finger_y[stem],finger_y[stem]-.004,finger_y[stem]-.010,finger_y[stem]-.023]
                b.head=(sign*xs[j-1],ys[j-1],zs[j-1]);b.tail=(sign*xs[j],ys[j],zs[j])
            b.align_roll(Vector((-sign,0,0)))
bpy.ops.object.mode_set(mode='OBJECT')
for p in meta.pose.bones:
    if p.rigify_type=='limbs.super_finger':p.rigify_parameters.rotation_axis='x'
if not bpy.context.preferences.addons.get('rigify'):bpy.ops.preferences.addon_enable(module='rigify')
bpy.ops.pose.rigify_generate()
control=bpy.context.object;control.name='MiniBotC_ControlRig';control.show_in_front=True
for p in control.pose.bones:
    p.matrix_basis=Matrix.Identity(4)
    if 'IK_Stretch' in p:p['IK_Stretch']=0.0
    if 'stretch_length' in p:p['stretch_length']=1.0
    if 'IK_FK' in p:p['IK_FK']=0.0
meta.hide_set(True);meta.hide_render=True
data=bpy.data.armatures.new('MiniBotC_Humanoid');rig=bpy.data.objects.new('MiniBotC_Humanoid',data);scene.collection.objects.link(rig)
activate(rig);bpy.ops.object.mode_set(mode='EDIT')
for name,src in mapping.items():
    ref=control.data.bones[src];b=data.edit_bones.new(name);b.head=ref.head_local;b.tail=ref.tail_local;b.matrix=ref.matrix_local.copy()
    b.use_deform=name!='Root'
    if parents[name]:b.parent=data.edit_bones[parents[name]]
b=data.edit_bones.new('Socket_Weapon_R');b.head=(-.377,.048,.708);b.tail=(-.377,-.052,.708);b.parent=data.edit_bones['RightHand'];b.use_deform=True;b.align_roll(Vector((0,0,1)))
bpy.ops.object.mode_set(mode='OBJECT')
for name,src in mapping.items():
    c=rig.pose.bones[name].constraints.new('COPY_TRANSFORMS');c.target=control;c.subtarget=src;c.owner_space=c.target_space='POSE';c.name='Evaluated Rigify pose'
rig['humanoid_mapping']=json.dumps(mapping);rig['binding']='Linear blend skinning; maximum four influences; original Tripo fingers'
split_report={}
for level in range(3):
    o=bpy.data.objects[f'Player_LOD{level}'];split_report[o.name]=split_finger_bridges(o)
body=bpy.data.objects['Player_LOD0'];SkinBody(body,rig)
names=[b.name for b in data.bones if b.use_deform and 'Socket' not in b.name]
v=coords(body);x,y,z=v.T;w=np.zeros((len(v),len(names)),np.float32)
for vertex in body.data.vertices:
    for g in vertex.groups:
        n=body.vertex_groups[g.group].name
        if n in names:w[vertex.index,names.index(n)]=g.weight
finger_labels=np.full(len(v),-1,np.int32)
parts=np.array([p.value for p in body.data.attributes['rig_part'].data])
for side,sign in [('Left',1),('Right',-1)]:
    ids=np.flatnonzero((x*sign>.285)&(z>.565)&(z<.758))
    points=v[ids]
    labels=['Index','Middle','Ring','Pinky','Thumb']
    distances=[]
    for label in labels:
        chain=[]
        for j in range(1,4):
            b=data.bones[f'{side}Hand{label}{j}'];a=np.array(b.head_local);d=np.array(b.tail_local)-a
            t=np.clip(((points-a)*d).sum(axis=1)/(d*d).sum(),0,1)
            delta=points-(a+t[:,None]*d)
            delta[:,1]*=1.8 if label!='Thumb' else 1.2
            chain.append((delta*delta).sum(axis=1))
        distances.append(np.min(chain,axis=0))
    choice=np.argmin(distances,axis=0)
    palm=ids[parts[ids]<0];w[palm]=0;w[palm,names.index(side+'Hand')]=1
    for k,label in enumerate(labels):
        subset=ids[parts[ids]==k+(0 if sign>0 else 5)]
        base=data.bones[f'{side}Hand{label}1'].head_local.z
        finger_labels[subset]=k+(0 if sign>0 else 5)
        centers=[]
        for j in range(1,4):
            b=data.bones[f'{side}Hand{label}{j}'];centers.append((b.head_local.z+b.tail_local.z)*.5)
        w[subset]=0
        for idx in subset:
            zz=z[idx]
            if zz>=centers[0]:
                palm=np.clip((zz-(base-.008))/.016,0,1)
                w[idx,names.index(side+'Hand')]=palm;w[idx,names.index(f'{side}Hand{label}1')]=1-palm
            elif zz<=centers[2]:w[idx,names.index(f'{side}Hand{label}3')]=1
            else:
                j=0 if zz>=centers[1] else 1;t=(centers[j]-zz)/(centers[j]-centers[j+1])
                # 装甲中央を剛体に近づけ、節の周囲だけで曲げる。
                t=float(np.clip((t-.28)/.44,0,1));t=t*t*(3-2*t)
                w[idx,names.index(f'{side}Hand{label}{j+1}')]=1-t;w[idx,names.index(f'{side}Hand{label}{j+2}')]=t
w=write_weights(body,names,w);bind(body,rig)
np.savez_compressed(OUT/'SkinReference.npz',positions=v,weights=w,names=np.array(names),finger_labels=finger_labels)
from mathutils.bvhtree import BVHTree
bvh=BVHTree.FromPolygons([Vector(p) for p in v],[p.vertices[:] for p in body.data.polygons],all_triangles=True)
part_faces={label:[p.index for p in body.data.polygons if int(parts[p.vertices[0]])==label] for label in range(-1,10)}
part_bvh={label:BVHTree.FromPolygons([Vector(p) for p in v],[body.data.polygons[i].vertices[:] for i in ids],all_triangles=True) for label,ids in part_faces.items() if ids}
from mathutils.geometry import barycentric_transform
skin_report={}
for level in range(3):
    obj=bpy.data.objects[f'Player_LOD{level}']
    if level:
        values=np.zeros((len(obj.data.vertices),len(names)),np.float32)
        for vertex in obj.data.vertices:
            label=obj.data.attributes['rig_part'].data[vertex.index].value
            hit,normal,part_face,distance=part_bvh[label].find_nearest(vertex.co)
            face=part_faces[label][part_face]
            tri=body.data.polygons[face].vertices;pa,pb,pc=[Vector(v[i]) for i in tri]
            bary=barycentric_transform(hit,pa,pb,pc,Vector((1,0,0)),Vector((0,1,0)),Vector((0,0,1)))
            b=np.clip(np.array(bary),0,1);b/=b.sum();values[vertex.index]=sum(w[i]*s for i,s in zip(tri,b))
        write_weights(obj,names,values);bind(obj,rig)
    influences=[len([g for g in vert.groups if g.weight>0]) for vert in obj.data.vertices]
    sums=[sum(g.weight for g in vert.groups) for vert in obj.data.vertices]
    assert min(influences)>0 and max(influences)<=4 and max(abs(s-1) for s in sums)<1e-5
    skin_report[obj.name]={'vertices':len(obj.data.vertices),'max_influences':max(influences),'unweighted':0,'weight_sum_max_error':max(abs(s-1) for s in sums)}
    obj.hide_set(level!=0);obj.hide_render=level!=0
weapon=bpy.data.objects.new('Sword_Control',None);scene.collection.objects.link(weapon)
weapon.empty_display_type='CIRCLE';weapon.empty_display_size=.09;weapon.parent=rig;weapon.parent_type='BONE';weapon.parent_bone='Socket_Weapon_R'
update();weapon.matrix_world=Matrix.Translation(Vector((-.377,.048,.708)))@Vector((0,0,1)).rotation_difference(Vector((0,-1,0))).to_matrix().to_4x4()
weapon['usage']='Right-hand weapon offset. Animate this control for grip adjustments.'
for level in range(3):
    obj=bpy.data.objects[f'Sword_LOD{level}'];obj.parent=weapon;obj.matrix_parent_inverse=Matrix.Identity(4);obj.matrix_basis=Matrix.Identity(4)
grip=bpy.data.objects.new('LeftHand_GripTarget',None);scene.collection.objects.link(grip);grip.parent=weapon;grip.location=(0,0,-.11)
grip.empty_display_type='ARROWS';grip.empty_display_size=.055
update();grip.rotation_euler=(weapon.matrix_world.inverted().to_quaternion()@control.pose.bones['hand_ik.L'].matrix.to_quaternion()).to_euler()
control['two_hand_grip']=0.0;control.id_properties_ui('two_hand_grip').update(min=0,max=1,description='左手を剣の補助グリップへ追従させる。0=自由、1=両手持ち')
c=control.pose.bones['hand_ik.L'].constraints.new('COPY_TRANSFORMS');c.name='Two hand grip';c.target=grip;c.influence=0
dr=c.driver_add('influence').driver;dr.expression='grip';var=dr.variables.new();var.name='grip';var.type='SINGLE_PROP';var.targets[0].id=control;var.targets[0].data_path='["two_hand_grip"]'

cd=bpy.data.armatures.new('Companion_Rig');cr=bpy.data.objects.new('Companion_Rig',cd);scene.collection.objects.link(cr);cr.location=(-.85,0,1.10)
activate(cr);bpy.ops.object.mode_set(mode='EDIT')
for name,head,tail,parent in [('Root',(0,0,0),(0,0,.13),None),('Body',(0,0,0),(0,0,.18),'Root'),('Pod.L',(.20,0,0),(.29,0,0),'Body'),('Pod.R',(-.20,0,0),(-.29,0,0),'Body')]:
    b=cd.edit_bones.new(name);b.head=head;b.tail=tail;b.use_deform=name!='Root'
    if parent:b.parent=cd.edit_bones[parent]
bpy.ops.object.mode_set(mode='OBJECT');cr.show_in_front=True
for level in range(3):
    o=bpy.data.objects[f'Companion_LOD{level}'];v2=coords(o);weights=np.zeros((len(v2),4),np.float32);weights[:,1]=1
    for sign,index in [(1,2),(-1,3)]:
        t=np.clip((v2[:,0]*sign-.188)/.035,0,1);t*=abs(v2[:,2])<.095
        weights[:,index]=t;weights[:,1]-=t
    write_weights(o,['Root','Body','Pod.L','Pod.R'],weights);bind(o,cr)
cr['usage']='Root: travel; Body: hover and tilt; Pod.L/R: side pods. Local Z is up.'
rig.hide_set(True);rig.hide_render=True
for obj in [weapon,grip]:obj.hide_render=True
scene.render.fps=30;scene.frame_start=1;scene.frame_end=90;scene.frame_set(1)
for level in range(3):bpy.data.collections[f'TRIPO_LOD{level}'].hide_render=level!=0
report={'player_bones':len(rig.data.bones),'control_bones':len(control.data.bones),'companion_bones':len(cr.data.bones),'humanoid_mapping':mapping,'skin':skin_report,'finger_repair':split_report,'source':'MiniBotC_TripoOptimized.blend','geometry_policy':'Finger boundary vertices split without changing faces, UVs, or rest positions; original Tripo textures retained','controls':{'arms':['hand_ik.L','hand_ik.R'],'feet':['foot_ik.L','foot_ik.R'],'body':['root','torso','chest','hips','head'],'grip':'two_hand_grip','weapon':'Sword_Control','companion':'Companion_Rig'}}
(ART/'RigReport.json').write_text(json.dumps(report,indent=2),encoding='utf8')
activate(control);bpy.ops.object.mode_set(mode='POSE')
for c in control.data.collections_all:c.is_visible=c.name not in ['ORG','MCH','DEF'] and '(Tweak)' not in c.name and '(FK)' not in c.name
bpy.ops.pose.select_all(action='DESELECT');control.data.bones.active=control.data.bones['hand_ik.R'];control.pose.bones['hand_ik.R'].select=True
scene['pipeline_stage']='rigged_pending_pose_validation';scene['animation_status']='Rigged; no inherited motions'
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'01_Rigged.blend'),compress=True)
print('RIG BUILT '+json.dumps(report),flush=True)
