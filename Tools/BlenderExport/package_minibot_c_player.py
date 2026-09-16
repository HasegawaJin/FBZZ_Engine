"""検証済みFBXのメタと新Controllerを構築する。既存アセットはこの工程では変更しない。"""

import csv
import json
import tomllib
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STAGE = ROOT / 'Temp/PlayerExport_20260914_v2'
PACKAGE = STAGE / 'Player'
ANIMATION = STAGE / 'Animation_Player'


def guid_meta(path):
    meta = Path(str(path) + '.meta')
    if meta.exists():
        return tomllib.loads(meta.read_text(encoding='utf-8'))['meta']['guid']
    guid = uuid.uuid4().hex
    meta.write_text("[meta]\nguid = '" + guid + "'\n", encoding='utf-8')
    return guid


def derive_guid(source, key):
    def fold(value):
        for byte in (source + '/' + key).encode('utf-8'):
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        return f'{value:016x}'
    return fold(14695981039346656037) + fold(1469598103934665603)


def ref(guid, path):
    return 'guid:' + guid + '|' + path


def build(manifest):
    ANIMATION.mkdir(exist_ok=True)
    for path in [PACKAGE, ANIMATION, *sorted(PACKAGE.rglob('*'))]:
        if not path.name.endswith('.meta'):
            guid_meta(path)
    options = manifest['import_options']
    clip_by_path = {c['fbx'].split('Models/Player/')[1]: c for c in manifest['clips']}
    for path in PACKAGE.rglob('*.fbx'):
        guid = guid_meta(path)
        relative = path.relative_to(PACKAGE).as_posix()
        clip = clip_by_path.get(relative)
        meta = f"file_format_version = 1\n\n[meta]\nguid = '{guid}'\n\n[model]\n"
        meta += "importer = 'ModelImporter'\nimporter_version = 4\nsource_dcc = 'Blender'\nup_axis = 0\n"
        meta += "normal_map_convention = 'OpenGL'\nunit_scale_multiplier = 1.0\ngenerate_normals = true\ngenerate_tangents = true\n"
        meta += "generate_tex_descriptors = true\ndefault_compression = 'Auto'\nroot_motion_node = ''\nselected_meshes = []\nselected_animations = []\n"
        if clip:
            label = clip['take']
            meta += f"\n[[model.clips]]\nname = '{label}'\nloop = {str(clip['loop']).lower()}\nstart_frame = 0.0\nend_frame = -1.0\noutput_name = '{label}'\n"
            clip['source_guid'] = guid
            clip['runtime_guid'] = derive_guid(guid, f'anims/{label}.anim')
            clip['runtime_path'] = f'Library/Baked/{guid}/anims/{label}.anim'
            clip['runtime_ref'] = ref(clip['runtime_guid'], clip['runtime_path'])
        else:
            meta += 'clips = []\n'
        Path(str(path) + '.meta').write_text(meta, encoding='utf-8')
    player_guid = guid_meta(PACKAGE / 'Player.fbx')
    bones = json.loads((STAGE / 'ValidationReferences/Skeletons.json').read_text())['Player']
    masks = {'Player_Base': ['Root'], 'M_UpperBody': ['Spine'], 'M_Arms': ['LeftShoulder', 'RightShoulder']}
    mask_report = {}
    for name, roots in masks.items():
        path = ANIMATION / (name + '.mask')
        text = f"[mask]\nversion = 1\nname = '{name}'\ndefault_include = false\nskeleton_source = '{ref(player_guid, 'Assets/Models/Player/Player.fbx')}'\nskeleton_source_signature = ''\n"
        for bone in roots:
            assert bone in bones
            text += f"\n[[entries]]\nbone = '{bone}'\ninclude_children = true\nblend_depth = 0\nweight = 1.0\n"
        path.write_text(text, encoding='utf-8')
        guid_meta(path)
        included = []
        for bone in bones:
            ancestors, current = [], bone
            while current:
                ancestors.append(current)
                current = bones[current]['parent']
            if set(roots).intersection(ancestors):
                included.append(bone)
        assert 'Socket_Weapon_R' in included
        if name != 'Player_Base':
            assert not set(included).intersection({'Root', 'Hips', 'LeftFoot', 'RightFoot'})
        mask_report[name] = included
    assert len(mask_report['Player_Base']) == 54
    clips = {c['take']: c for c in manifest['clips']}
    attacks = ['Slash01', 'HighSpinAttack', 'JumpAttack', 'SlideAttack', 'UnderSlash', 'UnderSlashandUpperSlash']
    labels = ['Locomotion'] + [c['take'] for c in manifest['clips'] if c['take'] not in ('Idle', 'SwordWalk', 'Run_F')]
    triggers = ['Jump', 'Dodge', 'Stop', *attacks, 'GuardHit', 'Hit', 'Death', 'Victory', 'Defeat', 'Reset']
    parameters = [('Speed', 0, False), ('VerticalSpeed', 0, False), ('IsGrounded', 2, True), ('IsBlocking', 2, False)]
    parameters += [(name, 3, False) for name in triggers]
    base_ref = ref(guid_meta(ANIMATION / 'Player_Base.mask'), 'Assets/Animation/Player/Player_Base.mask')
    text = f"version = 6\ndefaultStateName = 'Locomotion'\nbaseLayerMaskPath = '{base_ref}'\nanyStateTransitions = []\nlayers = []\n"
    for name, kind, initial in parameters:
        text += f"\n[[parameters]]\nname = '{name}'\ntype = {kind}\nfloatValue = 0.0\nintValue = 0\nboolValue = {str(initial).lower()}\n"
    terminal = {'Death', 'Victory', 'DefeatIdle'}
    all_transitions = {}
    for label in labels:
        clip = clips.get(label)
        text += f"\n[[states]]\nname = '{label}'\nmode = {1 if label == 'Locomotion' else 0}\n"
        text += f"sourcePath = '{clip['runtime_ref'] if clip else ''}'\nclipName = '{label if clip else ''}'\nclipIndex = -1\n"
        text += f"loop = {str(clip['loop'] if clip else True).lower()}\nspeed = 1.0\nikWeight = 0.0\n"
        if label == 'Locomotion':
            text += "\n[states.blendTree1D]\nparamName = 'Speed'\ndampTime = 0.08\nsyncNormalizedTime = true\n"
            for name, threshold in [('Idle', 0.), ('SwordWalk', 2.), ('Run_F', 6.)]:
                text += f"\n[[states.blendTree1D.motions]]\nsourcePath = '{clips[name]['runtime_ref']}'\nclipName = '{name}'\nclipIndex = -1\nthreshold = {threshold}\nspeed = 1.0\nikWeight = 0.0\nposX = 0.0\nposY = 0.0\n"
        transitions = []

        def add(target, conditions=(), exit_time=None, duration=.08):
            transitions.append({'target': target, 'conditions': list(conditions), 'exit_time': exit_time, 'duration': duration})

        def on(name):
            return (name, 4, 0.)

        if label in terminal:
            add('Locomotion', [on('Reset')], duration=.15)
        else:
            for target, trigger in [('Death', 'Death'), ('DefeatIdle', 'Defeat'), ('Victory', 'Victory'), ('Hit', 'Hit')]:
                if label != target:
                    add(target, [on(trigger)], duration=.06 if target in ('Death', 'Hit') else .15)
            if label == 'Locomotion':
                add('JumpStart', [on('Jump')], duration=.04)
                add('Dodge', [on('Dodge')], duration=.04)
                add('RunStop', [on('Stop')], duration=.06)
                add('FallLoop', [('IsGrounded', 5, 0.), ('VerticalSpeed', 1, -.1)], duration=.08)
                for attack in attacks:
                    add(attack, [on(attack)], duration=.06)
                add('ToBlocking', [on('IsBlocking')])
            elif label == 'JumpStart':
                add('FallLoop', exit_time=1., duration=.06)
            elif label == 'FallLoop':
                add('Land', [on('IsGrounded')], duration=.04)
                add('JumpAttack', [on('JumpAttack')], duration=.06)
            elif label == 'ToBlocking':
                add('Blocking', [on('IsBlocking')], exit_time=1., duration=.04)
                add('BlockingToIdle', [('IsBlocking', 5, 0.)], exit_time=1., duration=.04)
            elif label == 'Blocking':
                add('GuardHit', [on('GuardHit')], duration=.025)
                add('BlockingToIdle', [('IsBlocking', 5, 0.)], duration=.06)
                add('Dodge', [on('Dodge')], duration=.04)
            elif label == 'GuardHit':
                add('Blocking', [on('IsBlocking')], exit_time=1., duration=.06)
                add('BlockingToIdle', [('IsBlocking', 5, 0.)], exit_time=1., duration=.06)
            else:
                add('Locomotion', [on('IsGrounded')], exit_time=1., duration=.10)
                add('FallLoop', [('IsGrounded', 5, 0.)], exit_time=1., duration=.06)
        all_transitions[label] = transitions
        for transition in transitions:
            target, exit_time = transition['target'], transition['exit_time']
            text += f"\n[[states.transitions]]\ntoStateName = '{target}'\nhasExitTime = {str(exit_time is not None).lower()}\nexitTime = {exit_time if exit_time is not None else 1.0}\nfixedDuration = true\ntransitionDuration = {transition['duration']}\n"
            for name, operation, threshold in transition['conditions']:
                text += f"\n[[states.transitions.conditions]]\nparamName = '{name}'\nop = {operation}\nthreshold = {threshold}\n"
    text += "\n[editorLayout]\nentryX = -240.0\nentryY = 80.0\nanyStateX = -240.0\nanyStateY = 240.0\n"
    for index, label in enumerate(labels):
        text += f"\n[[editorLayout.nodes]]\nstateName = '{label}'\nx = {float(index//5 * 340)}\ny = {float(index%5 * 180)}\n"
    controller = ANIMATION / 'Player.animcontroller'
    controller.write_text(text, encoding='utf-8')
    guid_meta(controller)
    parsed = tomllib.loads(text)
    state_names = {s['name'] for s in parsed['states']}
    param_names = {p['name'] for p in parsed['parameters']}
    clip_names = set()
    for state in parsed['states']:
        if state['clipName']:
            clip_names.add(state['clipName'])
        for motion in state.get('blendTree1D', {}).get('motions', []):
            clip_names.add(motion['clipName'])
        for transition in state.get('transitions', []):
            assert transition['toStateName'] in state_names
            assert all(c['paramName'] in param_names for c in transition.get('conditions', []))
    assert clip_names == set(clips)
    reachable = {'Locomotion'}
    while True:
        expanded = reachable | {t['target'] for name in reachable for t in all_transitions[name]}
        if expanded == reachable:
            break
        reachable = expanded
    assert reachable == state_names
    report = {'states': len(state_names), 'clips': len(clip_names), 'parameters': len(param_names),
              'transitions': sum(len(v) for v in all_transitions.values()), 'masks': mask_report,
              'all_states_reachable': True, 'runtime_clip_import_pending': True,
              'runtime_refs': {name: clip['runtime_ref'] for name, clip in clips.items()}}
    (STAGE / 'ControllerValidation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (PACKAGE / 'ExportManifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


if __name__ == '__main__':
    validation = json.loads((STAGE / 'ValidationReport.json').read_text())
    assert len(validation['models']) == 9 and len(validation['clips']) == 22
    manifest = json.loads((PACKAGE / 'ExportManifest.json').read_text(encoding='utf-8'))
    result = build(manifest)
    print(json.dumps({k: v for k, v in result.items() if k not in ('masks', 'runtime_refs')} | {'masks': {k: len(v) for k,v in result['masks'].items()}}, indent=2))
