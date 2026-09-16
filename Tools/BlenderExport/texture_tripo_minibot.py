"""生成メッシュの指を可動化し、UV とゲーム用 PBR テクスチャを作る。

Author: Hasegawa Jin
Date: 2026-09-13
"""

import bpy
import bmesh
import json
import math
import time
import numpy as np
from pathlib import Path
from mathutils import Vector, Matrix

ROOT = Path(r"C:\Users\jinhs\Downloads\FBZZ_Engine")
SRC = ROOT / 'GreenWare/Assets/_src/MiniBotC/GameReady'
TEX = ROOT / 'GreenWare/Assets/Textures/MiniBotC_GameReady'
TEX.mkdir(parents=True, exist_ok=True)
START = time.time()
scene = bpy.data.scenes['MiniBotC_GameReady']
bpy.context.window.scene = scene


def Log(message):
    print(f'TEXTURE [{time.time()-START:.1f}s] {message}', flush=True)


def Activate(obj):
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT')
    obj.hide_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def MeshObject(name, verts, faces, bone):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.data.collections['GAME_LOD0'].objects.link(obj)
    group = obj.vertex_groups.new(name=bone)
    group.add(list(range(len(verts))), 1, 'REPLACE')
    for p in mesh.polygons:
        p.use_smooth = True
    return obj


def FingerLink(name, a, b, radius, bone):
    axis = (b-a).normalized()
    u = axis.cross(Vector((0, 1, 0))).normalized()
    v = axis.cross(u).normalized()
    rings = [(.03, .70), (.14, 1), (.82, .95), (.95, .67)]
    verts = []
    count = 12
    for t, r in rings:
        for i in range(count):
            angle = i*math.tau/count
            verts.append(a.lerp(b, t) + radius*r*(u*math.cos(angle)+v*math.sin(angle)))
    faces = [tuple(reversed(range(count))), tuple(range((len(rings)-1)*count, len(rings)*count))]
    for j in range(len(rings)-1):
        for i in range(count):
            n = (i+1)%count
            faces.append((j*count+i,j*count+n,(j+1)*count+n,(j+1)*count+i))
    return MeshObject(name, verts, faces, bone)


def FingerJoint(name, point, radius, bone):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=8, radius=radius)
    bmesh.ops.translate(bm, vec=point, verts=list(bm.verts))
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.data.collections['GAME_LOD0'].objects.link(obj)
    obj.vertex_groups.new(name=bone).add(list(range(len(mesh.vertices))),1,'REPLACE')
    for p in mesh.polygons:
        p.use_smooth=True
    return obj


def RepairFingers(player):
    bm = bmesh.new()
    bm.from_mesh(player.data)
    relevant = [f for f in bm.faces if any(abs(v.co.x)>.345 and .54<v.co.z<.78 for v in f.verts)]
    edges = set(e for f in relevant for e in f.edges)
    verts = set(v for f in relevant for v in f.verts)
    bmesh.ops.bisect_plane(bm, geom=list(verts)+list(edges)+relevant,
                         plane_co=(0,0,.715), plane_no=(0,0,1), dist=.000001)
    doomed = [f for f in bm.faces if abs(f.calc_center_median().x)>.357 and .50<f.calc_center_median().z<.715]
    bmesh.ops.delete(bm,geom=doomed,context='FACES')
    for sign in [-1,1]:
        border=[e for e in bm.edges if e.is_boundary and all(v.co.x*sign>.35 and abs(v.co.z-.715)<.00001 for v in e.verts)]
        if border:
            bmesh.ops.holes_fill(bm,edges=border,sides=0)
    bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces))
    bm.to_mesh(player.data)
    bm.free()
    player.data.update()
    objects, landmarks = [], {}
    for side, sign in [('Left',1),('Right',-1)]:
        for label, y, length, radius in [('Index',-.003,.118,.012),('Middle',.027,.132,.0125),
                                         ('Ring',.057,.124,.0117),('Pinky',.085,.101,.010)]:
            root=Vector((sign*.410,y,.727))
            points=[root,root+Vector((sign*.010,-.002,-length*.40)),
                    root+Vector((sign*.003,-.008,-length*.74)),
                    root+Vector((-sign*.016,-.020,-length))]
            for j in range(3):
                bone=f'{side}Hand{label}{j+1}'
                landmarks[bone]=[list(points[j]),list(points[j+1])]
                objects.append(FingerLink(bone+'_Shell',points[j],points[j+1],radius*(1-j*.09),bone))
                objects.append(FingerJoint(bone+'_Joint',points[j],radius*.86,bone))
    Activate(objects[0])
    for o in objects:
        o.select_set(True)
    bpy.ops.object.join()
    fingers=bpy.context.object
    fingers.name='Player_Fingers_LOD0'
    fingers['finger_landmarks']=json.dumps(landmarks)
    fingers['repair']='Four merged fingers per hand replaced by articulated mechanical digits; source preserved.'
    return fingers


def Palette(xyz, kind):
    x,y,z=xyz.T
    a=np.abs(x)
    count=len(x)
    base=np.tile(np.array([.72,.755,.77,1],np.float32),(count,1))
    orm=np.tile(np.array([1,.31,.28,1],np.float32),(count,1))
    emit=np.zeros((count,4),np.float32);emit[:,3]=1
    def Set(mask,color,rough,metal,light=False):
        base[mask,:3]=color
        orm[mask,1:3]=(rough,metal)
        if light:emit[mask,:3]=(.10,1.0,.004)
    dark=np.array([.020,.027,.032])
    green=np.array([.09,.80,.004])
    if kind=='Fingers':
        Set(np.ones(count,bool),dark,.39,.70)
        return base,orm,emit
    if kind=='Player':
        front=y<-.045
        visor=front&(a<.148)&(z<1.383+.47*a)&(z>1.273+.18*a)
        Set(visor,[.006,.011,.013],.18,.50)
        Set((a<.076)&(z>1.198)&(z<1.268),dark,.42,.68)
        Set((a<.121)&(z>.912)&(z<1.012),dark,.43,.65)
        Set((a>.124)&(a<.184)&(z>.80)&(z<.89),dark,.44,.64)
        Set((a>.20)&(a<.298)&(z>1.017)&(z<1.105),dark,.40,.65)
        Set((a>.285)&(z<.767),dark,.41,.72)
        Set((a>.342)&(z>.710)&(z<.772)&(y>.025),[.62,.66,.69],.32,.42)
        for ax,cy,cz,rad in [(.269,.029,.981,.039),(.157,.010,.553,.042),(.218,.045,.130,.040)]:
            d=(a-ax)**2+(y-cy)**2+(z-cz)**2
            Set(d<rad**2,dark,.38,.78)
        Set((z<.031)|((z<.090)&(y<-.135)),dark,.42,.68)
        u=(a-.089)*.72+(z-1.374)*.694
        v=-(a-.089)*.694+(z-1.374)*.72
        Set(front&((u/.058)**2+(v/.019)**2<1),green,.20,.26,True)
        core=front&((x/.029)**2+((z-1.101)/.038)**2<1)
        Set(core,dark,.32,.65)
        Set(front&(np.abs(x)<.013+np.clip((z-1.080),0,.035)*.15)&(z>1.080)&(z<1.117),green,.24,.30,True)
        Set((y>.105)&(a<.019)&(z>1.080)&(z<1.137),dark,.34,.65)
        Set((y>.115)&(a<.010)&(z>1.090)&(z<1.127),green,.25,.30,True)
        Set((a>.176)&(((y-.006)/.022)**2+((z-1.403)/.024)**2<1),green,.25,.40,True)
        fin_center=.135+(z-1.465)*.29
        Set((z>1.53)&(z<1.644)&(np.abs(a-fin_center)<.009)&(y<.020),green,.25,.36,True)
        for ax,cy,cz in [(.199,.013,.552),(.260,.044,.130)]:
            Set((a>ax)&(((y-cy)/.023)**2+((z-cz)/.024)**2<1),green,.22,.50,True)
        Set(front&(np.abs(a-.148)<.006)&(z>.655)&(z<.677),green,.28,.35,True)
        Set((y<-.026)&(np.abs(a-(.337+(.905-z)*.52))<.004)&(z>.855)&(z<.902),green,.27,.36,True)
    else:
        Set(np.ones(count,bool),[.044,.053,.061],.32,.85)
        Set(z<.390,[.025,.029,.033],.53,.18)
        Set((z<.080)|((z>.376)&(z<.440)),[.31,.25,.16],.30,.86)
        blade=z>.440
        Set(blade,green,.25,.50,True)
        Set((z<.071)&(np.abs(x)<.005)&(y<-.020),green,.26,.40,True)
        Set((z>.393)&(z<.408)&(np.abs(x)<.018)&(y<-.020),green,.26,.40,True)
    return base,orm,emit


def ColorAttributes(obj,kind):
    mesh=obj.data
    coords=np.empty(len(mesh.vertices)*3,np.float32)
    mesh.vertices.foreach_get('co',coords)
    coords=coords.reshape(-1,3)
    indices=np.empty(len(mesh.loops),np.int32)
    mesh.loops.foreach_get('vertex_index',indices)
    arrays=Palette(coords[indices],kind)
    for name,data in zip(['Paint_Base','Paint_ORM','Paint_Emission'],arrays):
        attr=mesh.color_attributes.new(name=name,type='FLOAT_COLOR',domain='CORNER')
        attr.data.foreach_set('color',data.reshape(-1))


def Unwrap(obj):
    Activate(obj)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(72),island_margin=.003,
                             margin_method='FRACTION',area_weight=.20,scale_to_bounds=False)
    bpy.ops.object.mode_set(mode='OBJECT')
    obj.data.uv_layers.active.name='UVMap'


def Material(obj,label):
    mat=bpy.data.materials.new(label+'_PBR')
    mat.use_nodes=True
    obj.data.materials.clear();obj.data.materials.append(mat)
    return mat


def BakeMap(obj,mat,label,channel,resolution):
    nt=mat.node_tree;nodes=nt.nodes;nodes.clear()
    output=nodes.new('ShaderNodeOutputMaterial')
    shader=nodes.new('ShaderNodeEmission')
    attr=nodes.new('ShaderNodeVertexColor')
    attr.layer_name={'BaseColor':'Paint_Base','ORM':'Paint_ORM','Emission':'Paint_Emission'}[channel]
    nt.links.new(attr.outputs['Color'],shader.inputs['Color'])
    if channel=='ORM':
        sep=nodes.new('ShaderNodeSeparateColor');sep.mode='RGB'
        combine=nodes.new('ShaderNodeCombineColor');combine.mode='RGB'
        ao=nodes.new('ShaderNodeAmbientOcclusion');ao.samples=8
        ao.inputs['Distance'].default_value=.035
        nt.links.new(attr.outputs['Color'],sep.inputs[0])
        nt.links.new(ao.outputs['AO'],combine.inputs[0])
        nt.links.new(sep.outputs[1],combine.inputs[1]);nt.links.new(sep.outputs[2],combine.inputs[2])
        nt.links.new(combine.outputs[0],shader.inputs['Color'])
    elif channel=='BaseColor':
        noise=nodes.new('ShaderNodeTexNoise');noise.inputs['Scale'].default_value=180
        noise.inputs['Detail'].default_value=2
        remap=nodes.new('ShaderNodeMapRange')
        remap.inputs['To Min'].default_value=.955;remap.inputs['To Max'].default_value=1.035
        mix=nodes.new('ShaderNodeMixRGB');mix.blend_type='MULTIPLY';mix.inputs[0].default_value=1
        nt.links.new(noise.outputs['Fac'],remap.inputs['Value'])
        nt.links.new(attr.outputs['Color'],mix.inputs[1]);nt.links.new(remap.outputs[0],mix.inputs[2])
        nt.links.new(mix.outputs[0],shader.inputs['Color'])
    nt.links.new(shader.outputs[0],output.inputs['Surface'])
    image=bpy.data.images.new(label+'_'+channel,width=resolution,height=resolution,alpha=False)
    image.colorspace_settings.name='Non-Color' if channel=='ORM' else 'sRGB'
    target=nodes.new('ShaderNodeTexImage');target.image=image;nodes.active=target
    Activate(obj)
    scene.render.bake.use_selected_to_active=False
    scene.render.bake.margin=8
    scene.cycles.samples=8 if channel=='ORM' else 1
    Log('Bake '+label+' '+channel)
    bpy.ops.object.bake(type='EMIT')
    image.filepath_raw=str(TEX/(label+'_'+channel+'.png'));image.file_format='PNG';image.save()
    return image


def BakeNormal(obj,high,mat,label,resolution):
    image=bpy.data.images.new(label+'_Normal',width=resolution,height=resolution,alpha=False)
    image.colorspace_settings.name='Non-Color'
    nt=mat.node_tree
    target=nt.nodes.new('ShaderNodeTexImage');target.image=image;nt.nodes.active=target
    Activate(obj)
    high.hide_render=False;high.hide_set(False);high.select_set(True)
    bpy.data.collections['SOURCE_HighPoly'].hide_render=False
    scene.render.bake.use_selected_to_active=True
    scene.render.bake.cage_extrusion=.020
    scene.render.bake.max_ray_distance=.045
    scene.render.bake.normal_space='TANGENT'
    scene.render.bake.margin=8
    scene.cycles.samples=1
    Log('Bake '+label+' high-poly normals')
    bpy.ops.object.bake(type='NORMAL')
    high.hide_render=True;high.hide_set(True);high.select_set(False)
    bpy.data.collections['SOURCE_HighPoly'].hide_render=True
    scene.render.bake.use_selected_to_active=False
    image.filepath_raw=str(TEX/(label+'_Normal.png'));image.file_format='PNG';image.save()
    return image


def FinalMaterial(mat,maps):
    nt=mat.node_tree;nt.nodes.clear()
    output=nt.nodes.new('ShaderNodeOutputMaterial');output.location=(700,100)
    bsdf=nt.nodes.new('ShaderNodeBsdfPrincipled');bsdf.location=(420,100)
    nt.links.new(bsdf.outputs[0],output.inputs['Surface'])
    for i,(label,img) in enumerate(maps.items()):
        node=nt.nodes.new('ShaderNodeTexImage');node.image=img;node.label=label;node.location=(-400,360-i*280)
        if label=='BaseColor':nt.links.new(node.outputs['Color'],bsdf.inputs['Base Color'])
        elif label=='Emission':
            nt.links.new(node.outputs['Color'],bsdf.inputs['Emission Color'])
            bsdf.inputs['Emission Strength'].default_value=2.5
        elif label=='ORM':
            sep=nt.nodes.new('ShaderNodeSeparateColor');sep.mode='RGB';sep.location=(-70,-150)
            nt.links.new(node.outputs['Color'],sep.inputs[0])
            nt.links.new(sep.outputs[1],bsdf.inputs['Roughness']);nt.links.new(sep.outputs[2],bsdf.inputs['Metallic'])
        else:
            normal=nt.nodes.new('ShaderNodeNormalMap');normal.location=(110,-370)
            normal.inputs['Strength'].default_value=.7
            nt.links.new(node.outputs['Color'],normal.inputs['Color']);nt.links.new(normal.outputs[0],bsdf.inputs['Normal'])
    mat['texture_layout']='ORM: R=ambient occlusion, G=roughness, B=metallic; tangent normal OpenGL +Y'


def Main():
    player=bpy.data.objects['Player_LOD0']
    fingers=RepairFingers(player)
    Log('Independent fingers created')
    scene.render.engine='CYCLES'
    scene.cycles.device='CPU'
    preferences=bpy.context.preferences.addons['cycles'].preferences
    for backend in ['OPTIX','CUDA','HIP','ONEAPI']:
        try:
            preferences.compute_device_type=backend
            preferences.refresh_devices()
            devices=[d for d in preferences.devices if d.type==backend]
            if devices:
                for d in preferences.devices:d.use=d.type==backend
                scene.cycles.device='GPU'
                Log('Bake device: '+backend+' '+','.join(d.name for d in devices))
                break
        except (TypeError,RuntimeError):
            pass
    scene.world=bpy.data.worlds.new('GameReadyWorld')
    scene.world.use_nodes=True
    next(n for n in scene.world.node_tree.nodes if n.type=='BACKGROUND').inputs['Color'].default_value=(.20,.20,.20,1)
    scene.render.image_settings.file_format='PNG'
    scene.view_settings.view_transform='AgX'
    texture_report={}
    for obj,label,kind,resolution,high_name in [
        (player,'MiniBotC_Player','Player',2048,'SOURCE_Player'),
        (fingers,'MiniBotC_Fingers','Fingers',1024,None),
        (bpy.data.objects['Sword_LOD0'],'MiniBotC_Sword','Sword',2048,'SOURCE_Sword')]:
        for o in scene.objects:o.hide_render=True
        obj.hide_render=False
        Log('UV '+label)
        Unwrap(obj)
        ColorAttributes(obj,kind)
        mat=Material(obj,label)
        maps={channel:BakeMap(obj,mat,label,channel,resolution) for channel in ['BaseColor','ORM','Emission']}
        if high_name:maps['Normal']=BakeNormal(obj,bpy.data.objects[high_name],mat,label,resolution)
        FinalMaterial(mat,maps)
        texture_report[label]={'resolution':resolution,'maps':{k:v.filepath_raw for k,v in maps.items()},'triangles':sum(len(p.vertices)-2 for p in obj.data.polygons)}
        Log(label+' textures complete')
    for o in scene.objects:o.hide_render=o.name.startswith('SOURCE_')
    bpy.data.collections['SOURCE_HighPoly'].hide_render=True
    scene['pipeline_stage']='textured_and_finger_repair'
    (SRC/'TextureReport.json').write_text(json.dumps(texture_report,indent=2),encoding='utf8')
    bpy.ops.wm.save_as_mainfile(filepath=str(SRC/'02_Textured.blend'),compress=True)
    Log('TEXTURING COMPLETE')


if __name__ == "__main__":
    Main()
