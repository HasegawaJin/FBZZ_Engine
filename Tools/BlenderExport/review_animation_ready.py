"""最終骨格のポーズと全 LOD を同じ構図で描画して確認する。"""
import bpy,json
from pathlib import Path
from mathutils import Vector
ROOT=Path(r'C:\Users\jinhs\Downloads\FBZZ_Engine');OUT=ROOT/'Docs/Art/MiniBotC/AnimationReady'
scene=bpy.context.scene;control=bpy.data.objects['MiniBotC_ControlRig'];control.animation_data.action=bpy.data.actions['DEMO_RigCheck']
scene.render.engine='CYCLES';scene.cycles.device='GPU';scene.cycles.samples=20
prefs=bpy.context.preferences.addons['cycles'].preferences;prefs.compute_device_type='OPTIX';prefs.refresh_devices()
for d in prefs.devices:d.use=d.type=='OPTIX'
cam=scene.camera;cam.data.type='ORTHO';cam.location=(2.5,-5,2.25);cam.rotation_euler=(Vector((-.15,-.10,.87))-cam.location).to_track_quat('-Z','Y').to_euler();cam.data.ortho_scale=2.55
scene.render.resolution_x=1400;scene.render.resolution_y=1200;scene.render.resolution_percentage=100
for label,frame,level in [('Hero',46,0),('Hero_LOD1',46,1),('Hero_LOD2',46,2),('Final_TwoHand',61,0),('Final_ArmsUp',16,0)]:
    for i in range(3):
        bpy.data.collections[f'TRIPO_LOD{i}'].hide_render=i!=level
        for prefix in ['Player','Player_Fingers','Sword','Companion']:
            o=bpy.data.objects[f'{prefix}_LOD{i}'];o.hide_render=i!=level;o.hide_set(i!=level)
    scene.frame_set(frame);bpy.context.view_layer.update();scene.render.filepath=str(OUT/(label+'.png'));bpy.ops.render.render(write_still=True)
    print('FINAL PREVIEW '+label,flush=True)
