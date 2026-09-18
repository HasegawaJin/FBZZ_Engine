"""癒着した4指の可動部を節ごとに補修し、Tripo の色を元表面から引き継ぐ。"""
import bpy,bmesh,json,numpy as np
from pathlib import Path
from mathutils import Vector,Matrix
from mathutils.bvhtree import BVHTree
from mathutils.geometry import barycentric_transform
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine');rig=bpy.data.objects['MiniBotC_Humanoid']
scene=bpy.context.scene
if bpy.context.object and bpy.context.object.mode!='OBJECT':bpy.ops.object.mode_set(mode='OBJECT')
source=bpy.data.objects['Player_LOD0'].data.copy()
vs=[v.co.copy() for v in source.vertices];faces=[p.vertices[:] for p in source.polygons]
bvh=BVHTree.FromPolygons(vs,faces,all_triangles=True)
base=next(n.image for n in bpy.data.objects['Player_LOD0'].data.materials[0].node_tree.nodes if n.type=='TEX_IMAGE' and n.image and n.image.colorspace_settings.name=='sRGB')
pixels=np.empty(len(base.pixels),np.float32);base.pixels.foreach_get(pixels);pixels=pixels.reshape(base.size[1],base.size[0],4)
src_obj=bpy.data.objects.new('SOURCE_Finger_Texture',source);scene.collection.objects.link(src_obj)
for i,m in enumerate(source.materials):
    source.materials[i]=m.copy();sn=source.materials[i].node_tree
    principled=next(n for n in sn.nodes if n.type=='BSDF_PRINCIPLED');output=next(n for n in sn.nodes if n.type=='OUTPUT_MATERIAL')
    emission=sn.nodes.new('ShaderNodeEmission');sn.links.new(principled.inputs['Base Color'].links[0].from_socket,emission.inputs[0]);sn.links.new(emission.outputs[0],output.inputs['Surface'])
scene.render.engine='CYCLES';scene.cycles.samples=8;scene.cycles.device='CPU'
for o in scene.objects:
    if o.type=='MESH':o.hide_render=True
src_obj.hide_render=False
texture_dir=ROOT/'GreenWare/Assets/Textures/MiniBotC_AnimationReady';texture_dir.mkdir(parents=True,exist_ok=True)
mat=bpy.data.materials.new('Player_Fingers_Tripo');mat.use_nodes=True;nt=mat.node_tree
shader=next(n for n in nt.nodes if n.type=='BSDF_PRINCIPLED');shader.inputs['Roughness'].default_value=.5
tex=nt.nodes.new('ShaderNodeTexImage');tex.image=base;nt.links.new(tex.outputs['Color'],shader.inputs['Base Color'])
report={}
for level in range(3):
    body=bpy.data.objects[f'Player_LOD{level}'];bm=bmesh.new();bm.from_mesh(body.data);part=bm.verts.layers.int.get('rig_part')
    bad=[];armor=[];uvlayer=bm.loops.layers.uv.active
    for f in bm.faces:
        c=f.calc_center_median();label=f.verts[0][part]
        q=sum((loop[uvlayer].uv for loop in f.loops),Vector((0,0)))/len(f.loops)
        rgb=pixels[int(q.y*base.size[1])%base.size[1],int(q.x*base.size[0])%base.size[0],:3]
        if .690<c.z<.735 and abs(c.x)>.405 and rgb.mean()>.22:
            armor.extend(v for v in f.verts);continue
        if label>=0 and label%5<4:bad.append(f)
        elif .565<c.z<.706 and abs(c.x)>.367:bad.append(f)
    bmesh.ops.delete(bm,geom=bad,context='FACES')
    loose=[v for v in bm.verts if not v.link_faces];bmesh.ops.delete(bm,geom=loose,context='VERTS')
    bm.to_mesh(body.data);bm.free()
    for vertex in body.data.vertices:
        if .69<vertex.co.z<.745 and abs(vertex.co.x)>.405:
            for group in body.vertex_groups:group.remove([vertex.index])
            body.vertex_groups['LeftHand' if vertex.co.x>0 else 'RightHand'].add([vertex.index],1,'REPLACE')
    for attr in list(body.data.attributes):
        if attr.name=='rig_saved_normal':body.data.attributes.remove(attr)
    pieces=[]
    for side,sign in [('Left',1),('Right',-1)]:
        for label in ['Index','Middle','Ring','Pinky']:
            for j in range(1,4):
                name=f'{side}Hand{label}{j}';bone=rig.data.bones[name]
                length=bone.length;center=(bone.head_local+bone.tail_local)/2
                bpy.ops.mesh.primitive_cube_add(size=1,location=center)
                obj=bpy.context.object;obj.name='FingerSegment';obj.rotation_mode='QUATERNION';obj.rotation_quaternion=Vector((0,0,1)).rotation_difference(bone.tail_local-bone.head_local)
                obj.scale=(.031 if j==1 else .029 if j==2 else .026,.024 if label!='Pinky' else .021,max(.012,length-.0015))
                bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
                mod=obj.modifiers.new('Soft robot edges','BEVEL');mod.width=.005 if j<3 else .006;mod.segments=2 if level==0 else 1
                bpy.ops.object.modifier_apply(modifier=mod.name)
                for p in obj.data.polygons:p.use_smooth=False
                group=obj.vertex_groups.new(name=name);group.add(list(range(len(obj.data.vertices))),1,'REPLACE');pieces.append(obj)
                if j>1:
                    bpy.ops.mesh.primitive_uv_sphere_add(segments=8,ring_count=4,radius=.010,location=bone.head_local)
                    joint=bpy.context.object;g=joint.vertex_groups.new(name=name);g.add(list(range(len(joint.data.vertices))),1,'REPLACE');pieces.append(joint)
    bpy.ops.object.select_all(action='DESELECT')
    for obj in pieces:obj.select_set(True)
    bpy.context.view_layer.objects.active=pieces[0];bpy.ops.object.join();finger=bpy.context.object;finger.name=f'Player_Fingers_LOD{level}'
    bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    bm=bmesh.new();bm.from_mesh(finger.data);bmesh.ops.triangulate(bm,faces=list(bm.faces));bm.to_mesh(finger.data);bm.free()
    for layer in list(finger.data.uv_layers):finger.data.uv_layers.remove(layer)
    uv=finger.data.uv_layers.new(name='UVMap')
    for p in finger.data.polygons:
        for loop in p.loop_indices:
            v=finger.data.vertices[finger.data.loops[loop].vertex_index].co
            hit,normal,index,distance=bvh.find_nearest(v);tri=source.polygons[index]
            a,b,c=[vs[i] for i in tri.vertices]
            target=[Vector((*source.uv_layers.active.data[i].uv,0)) for i in tri.loop_indices]
            value=barycentric_transform(hit,a,b,c,*target);uv.data[loop].uv=value.xy
    finger.data.materials.clear();finger.data.materials.append(mat)
    bake_material=mat.copy();bake_material.name=f'Player_Fingers_LOD{level}_Tripo';finger.data.materials[0]=bake_material
    nt=bake_material.node_tree;shader=next(n for n in nt.nodes if n.type=='BSDF_PRINCIPLED')
    image=bpy.data.images.new(f'Player_Fingers_LOD{level}_BaseColor',width=1024 if level==0 else 512 if level==1 else 256,height=1024 if level==0 else 512 if level==1 else 256,alpha=False)
    image.colorspace_settings.name='sRGB';image.filepath_raw=str(texture_dir/(image.name+'.png'));image.file_format='PNG'
    image_node=nt.nodes.new('ShaderNodeTexImage');image_node.image=image;nt.nodes.active=image_node
    bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT');bpy.ops.uv.smart_project(angle_limit=.9,island_margin=.012);bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT');src_obj.select_set(True);finger.select_set(True);bpy.context.view_layer.objects.active=finger
    src_obj.hide_render=False;finger.hide_render=False
    scene.render.bake.use_selected_to_active=True;scene.render.bake.use_cage=True;scene.render.bake.cage_extrusion=.035;scene.render.bake.max_ray_distance=.10;scene.render.bake.margin=8
    bpy.ops.object.bake(type='EMIT');image.save();image.pack();nt.links.new(image_node.outputs['Color'],shader.inputs['Base Color'])
    for n in list(nt.nodes):
        if n.type=='TEX_IMAGE' and n!=image_node:nt.nodes.remove(n)
    finger.parent=rig;m=finger.modifiers.new('Rigid finger segments','ARMATURE');m.object=rig
    collection=bpy.data.collections[f'TRIPO_LOD{level}']
    for c in list(finger.users_collection):c.objects.unlink(finger)
    collection.objects.link(finger);finger.hide_set(level!=0);finger.hide_render=level!=0
    report[f'LOD{level}']={'body_triangles':len(body.data.polygons),'finger_triangles':len(finger.data.polygons),'finger_vertices':len(finger.data.vertices),'max_finger_influences':1,'material':'Original Tripo BaseColor sampled on the replacement joint surfaces'}
bpy.data.objects.remove(src_obj,do_unlink=True);bpy.data.meshes.remove(source)
for o in scene.objects:
    if o.type=='MESH' and '_LOD' in o.name:o.hide_render=not o.name.endswith('LOD0')
(ROOT/'Docs/Art/MiniBotC/AnimationReady/FingerRepair.json').write_text(json.dumps(report,indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GreenWare/Assets/_src/MiniBotC/AnimationReady/01_FingersRepaired.blend'),compress=True)
print('ARTICULATED FINGERS '+json.dumps(report),flush=True)
