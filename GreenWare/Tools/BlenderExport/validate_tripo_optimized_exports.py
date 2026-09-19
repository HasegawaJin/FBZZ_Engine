"""全 LOD の交換ファイルを再読込し、形状・UV・テクスチャ接続を確認する。"""

import bpy
import hashlib
import json
import struct
import numpy as np
from pathlib import Path
from mathutils import Vector
from mathutils.kdtree import KDTree

ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine')
OUT=ROOT/'GreenWare/Assets/Models/MiniBotC_Tripo'
report=json.loads((OUT/'AssetReport.json').read_text(encoding='utf8'))
scene=bpy.data.scenes['MiniBotC_TripoOptimized'];bpy.context.window.scene=scene
expected={}
for obj in scene.objects:
    if obj.type=='MESH' and obj.name.endswith(('LOD0','LOD1','LOD2')):
        expected[obj.name]={'vertices':[v.co.copy() for v in obj.data.vertices],
                            'triangles':sum(len(p.vertices)-2 for p in obj.data.polygons)}
for model in report['models'].values():
    texture=model['base_color']
    assert hashlib.sha256(Path(texture['file']).read_bytes()).hexdigest()==texture['sha256']

results={}
for name,ref in expected.items():
    tree=KDTree(len(ref['vertices']))
    for i,co in enumerate(ref['vertices']):tree.insert(co,i)
    tree.balance()
    for ext in ['fbx','glb']:
        path=OUT/(name+'.'+ext)
        bpy.ops.wm.read_factory_settings(use_empty=True)
        if ext=='fbx':bpy.ops.import_scene.fbx(filepath=str(path),use_anim=False)
        else:bpy.ops.import_scene.gltf(filepath=str(path))
        bpy.context.view_layer.update()
        objects=list(bpy.context.scene.objects)
        assert len(objects)==1 and objects[0].type=='MESH','Unexpected objects: '+path.name
        obj=objects[0]
        assert obj.name==name
        coords=[obj.matrix_world@v.co for v in obj.data.vertices]
        error=max(tree.find(co)[2] for co in coords)
        assert error<.00001,'Position/pivot changed: '+path.name
        count=sum(len(p.vertices)-2 for p in obj.data.polygons)
        assert count==ref['triangles'] and len(obj.data.materials)==1 and len(obj.data.uv_layers)==1
        uvs=np.array([item.uv[:] for item in obj.data.uv_layers.active.data])
        assert np.isfinite(uvs).all(),'Non-finite UV values'
        mat=obj.data.materials[0]
        shader=next(n for n in mat.node_tree.nodes if n.type=='BSDF_PRINCIPLED')
        assert shader.inputs['Base Color'].is_linked and shader.inputs['Normal'].is_linked,'Missing material connections'
        images={n.image for n in mat.node_tree.nodes if n.type=='TEX_IMAGE' and n.image}
        assert len(images)==2,'Unexpected textures'
        for image in images:
            assert image.packed_file or Path(bpy.path.abspath(image.filepath)).is_file(),'Missing texture file'
        entry={'triangles':count,'vertices':len(coords),'max_position_error_m':error,'uv_layers':1,'materials':1,'textures':2}
        if ext=='glb':
            raw=path.read_bytes();length=struct.unpack_from('<I',raw,12)[0];gltf=json.loads(raw[20:20+length])
            assert len(gltf['meshes'])==1 and not gltf.get('skins') and not gltf.get('animations')
            assert len(gltf['materials'])==1
            material=gltf['materials'][0]
            assert 'baseColorTexture' in material['pbrMetallicRoughness'] and 'normalTexture' in material
            assert not material.get('alphaMode') or material['alphaMode']=='OPAQUE'
            entry['glb_opaque_static_mesh']=True
        results[path.name]=entry
        print('PASS '+path.name+' '+json.dumps(entry),flush=True)
assert len(results)==18
output={'verified_exports':len(results),'source_color_images_unchanged':True,'exports':results,
        'runtime_engine_tested':False,'rig_binding':'New optimized meshes are not skinned'}
(ROOT/'Docs/Art/MiniBotC/TripoOptimized/ExportValidation.json').write_text(json.dumps(output,indent=2),encoding='utf8')
print('ALL 18 EXPORTS VERIFIED',flush=True)
