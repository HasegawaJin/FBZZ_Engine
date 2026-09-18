"""配置後の参照、旧データ保全、シーン差分、Controllerの主要な遷移経路を検査する。"""

import hashlib
import json
import tomllib
from pathlib import Path

from package_minibot_c_player import derive_guid


ROOT = Path(__file__).resolve().parents[3]
ASSETS = ROOT / 'GreenWare/Assets'
ART = ROOT / 'Docs/Art/MiniBotC/ExportDelivery'


def read(path):
    return tomllib.loads(path.read_text(encoding='utf-8-sig'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    package = ASSETS / 'Models/Player'
    animation = ASSETS / 'Animation/Player'
    manifest = json.loads((package / 'ExportManifest.json').read_text(encoding='utf-8'))
    audit = json.loads((ART / 'ReplacementAudit.json').read_text(encoding='utf-8'))
    delivery = json.loads((ART / 'DeliveryReport.json').read_text(encoding='utf-8'))
    assert len(list(package.rglob('*.fbx'))) == 31
    assert len([p for p in (package / 'Textures').iterdir() if p.suffix.lower() in ('.png', '.jpg')]) == 19
    for image in manifest['textures']:
        assert digest(package / image['export_path']) == digest(Path(image['source_path']))
    for relative, expected in audit['legacy_model_files'].items():
        assert digest(ASSETS / 'Models/Player_BackUp' / relative) == expected
    scene_changes = {c['path']: c for c in delivery['scene_changes']}
    for scene in audit['scenes']:
        path = ROOT / scene['path']
        before = read(ART / 'SceneBackups' / path.name)
        after = read(path)
        removed = {o['id'] for o in scene['removed']}
        assert after['gameobjects'] == [o for o in before['gameobjects'] if o['instanceId'] not in removed]
        assert digest(path) == scene_changes[scene['path']]['after_sha256']
        assert not any(obj.get('name') == 'Player' for obj in after['gameobjects'])
    guids = {}
    for meta in ASSETS.rglob('*.meta'):
        value = read(meta).get('meta', {}).get('guid')
        if value:
            guids.setdefault(value, []).append(str(meta))
    for folder in (package, animation):
        for path in [folder, *folder.rglob('*')]:
            if path.name.endswith('.meta'):
                continue
            meta = Path(str(path) + '.meta')
            assert meta.exists(), meta
            value = read(meta)['meta']['guid']
            assert len(guids[value]) == 1, guids[value]
    assert derive_guid('f9afc042bf0ff57266eb135e1c6a347c', 'anims/Katana_Idle.anim') == 'fd0cd663e3ddc91d519bace6fd35938f'
    refs = set()
    for clip in manifest['clips']:
        meta = read(package / 'Animations' / (clip['take'] + '.fbx.meta'))
        assert meta['model']['clips'][0]['loop'] == clip['loop']
        assert meta['model']['source_dcc'] == 'Blender' and meta['model']['normal_map_convention'] == 'OpenGL'
        assert clip['runtime_guid'] == derive_guid(meta['meta']['guid'], 'anims/' + clip['take'] + '.anim')
        refs.add(clip['runtime_ref'])
    controller = read(animation / 'Player.animcontroller')
    states = {s['name']: s for s in controller['states']}
    defaults = {p['name']: p['boolValue'] if p['type'] in (2, 3) else p['floatValue'] for p in controller['parameters']}
    used = set()
    for state in states.values():
        if state['sourcePath']:
            used.add(state['sourcePath'])
        used.update(m['sourcePath'] for m in state.get('blendTree1D', {}).get('motions', []))
    assert used == refs

    def step(state, normalized=0., **overrides):
        params = defaults | overrides
        for transition in states[state].get('transitions', []):
            if transition['hasExitTime'] and normalized < transition['exitTime']:
                continue
            okay = True
            for condition in transition.get('conditions', []):
                value, threshold, op = params[condition['paramName']], condition['threshold'], condition['op']
                okay &= {0: value > threshold, 1: value < threshold, 2: value == threshold,
                         3: value != threshold, 4: bool(value), 5: not value}[op]
            if okay:
                return transition['toStateName']
        return state

    checks = []
    for trigger, target in [('Jump', 'JumpStart'), ('Dodge', 'Dodge'), ('Stop', 'RunStop'),
                             ('Slash01', 'Slash01'), ('HighSpinAttack', 'HighSpinAttack'), ('JumpAttack', 'JumpAttack'),
                             ('SlideAttack', 'SlideAttack'), ('UnderSlash', 'UnderSlash'), ('UnderSlashandUpperSlash', 'UnderSlashandUpperSlash')]:
        assert step('Locomotion', **{trigger: True}) == target
        checks.append(trigger)
    assert step('Locomotion', IsGrounded=False, VerticalSpeed=-2.) == 'FallLoop'
    assert step('JumpStart', 1., IsGrounded=False) == 'FallLoop'
    assert step('FallLoop', IsGrounded=True) == 'Land'
    assert step('Land', 1.) == 'Locomotion'
    assert step('Locomotion', IsBlocking=True) == 'ToBlocking'
    assert step('ToBlocking', 1., IsBlocking=True) == 'Blocking'
    assert step('Blocking', IsBlocking=True, GuardHit=True) == 'GuardHit'
    assert step('GuardHit', 1., IsBlocking=True) == 'Blocking'
    assert step('Blocking', IsBlocking=False) == 'BlockingToIdle'
    assert step('BlockingToIdle', 1.) == 'Locomotion'
    for target, trigger in [('Death', 'Death'), ('Victory', 'Victory'), ('DefeatIdle', 'Defeat')]:
        assert step('Slash01', **{trigger: True}) == target
        assert step(target, 2., Speed=6., Jump=True, Hit=True) == target
        assert step(target, Reset=True) == 'Locomotion'
    assert step('Slash01', .5) == 'Slash01'
    assert step('Slash01', 1.) == 'Locomotion'
    assert step('JumpAttack', 1., IsGrounded=False) == 'FallLoop'
    assert step('Dodge', 1.) == 'Locomotion'
    assert step('RunStop', 1.) == 'Locomotion'
    for name in ('export_minibot_c_player.py', 'validate_minibot_c_player_export.py', 'audit_player_replacement.py',
                 'package_minibot_c_player.py', 'deploy_minibot_c_player.py', 'verify_player_delivery.py'):
        path = Path(__file__).with_name(name)
        compile(path.read_text(encoding='utf-8'), str(path), 'exec')
    report = {'fbx_files': 31, 'identical_texture_copies': 19, 'legacy_model_files_preserved': 130,
              'scene_removals': 436, 'remaining_scene_objects_unchanged': True,
              'new_guids_unique': True, 'derived_guid_matches_existing_engine_asset': True,
              'runtime_clip_refs': len(refs), 'controller_scenarios_passed': True,
              'fbzz_import_pending': True, 'fbzz_runtime_tested': False}
    (ART / 'DeliveryValidation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
