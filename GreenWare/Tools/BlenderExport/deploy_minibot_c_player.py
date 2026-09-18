"""検証済みPlayerを配置し、監査済みの旧Playerブロックだけをシーンから除く。"""

import csv
import hashlib
import json
import re
import shutil
import tomllib
from pathlib import Path

from package_minibot_c_player import guid_meta


ROOT = Path(__file__).resolve().parents[3]
ASSETS = ROOT / 'GreenWare/Assets'
STAGE = ROOT / 'Temp/PlayerExport_20260914_v2'
ART = ROOT / 'Docs/Art/MiniBotC/ExportDelivery'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked_move(source, destination):
    source, destination = source.resolve(), destination.resolve()
    assert source.is_relative_to(ROOT) and destination.is_relative_to(ROOT)
    assert source.exists() and not destination.exists(), (source, destination)
    source.rename(destination)


def main():
    audit = json.loads((ART / 'ReplacementAudit.json').read_text(encoding='utf-8'))
    validation = json.loads((STAGE / 'ValidationReport.json').read_text())
    bake = json.loads((STAGE / 'BakeReport.json').read_text())
    controller = json.loads((STAGE / 'ControllerValidation.json').read_text())
    assert bake['source_unchanged'] and len(validation['clips']) == 22 and controller['all_states_reachable']
    old = ASSETS / 'Models/Player'
    backup = ASSETS / 'Models/Player_BackUp'
    animation = ASSETS / 'Animation/Player'
    assert not backup.exists() and not backup.with_suffix('.meta').exists()
    for relative, expected in audit['legacy_model_files'].items():
        assert digest(old / relative) == expected, relative
    assert digest(old.with_suffix('.meta')) == audit['legacy_folder_meta_sha256']
    assert (STAGE / 'Player').is_dir() and (STAGE / 'Animation_Player').is_dir()
    animation_files = {p.relative_to(animation).as_posix(): digest(p) for p in animation.rglob('*') if p.is_file()}
    animation_folder_hash = digest(animation.with_suffix('.meta'))
    planned = []
    for entry in audit['scenes']:
        path = ROOT / entry['path']
        assert digest(path) == entry['sha256'], 'Scene changed since audit: ' + str(path)
        original = path.read_bytes()
        text = original.decode('utf-8-sig')
        removed = {o['id'] for o in entry['removed']}
        blocks = re.split(r'(?m)(?=^\[\[gameobjects\]\]\s*$)', text)
        kept = [blocks[0]]
        for block in blocks[1:]:
            obj = tomllib.loads(block)['gameobjects'][0]
            if obj['instanceId'] not in removed:
                kept.append(block)
        result = ''.join(kept)
        assert not any(identity in result for identity in removed), path
        assert not any(guid in result for guid in audit['legacy_guids']), path
        assert not re.search(r'MiniBot_Armature|SOCKET_Katana_|Models/WPN_Sword_[LR]', result), path
        before = tomllib.loads(text)
        after = tomllib.loads(result)
        expected_objects = [obj for obj in before['gameobjects'] if obj['instanceId'] not in removed]
        assert after['gameobjects'] == expected_objects
        assert {k:v for k,v in before.items() if k != 'gameobjects'} == {k:v for k,v in after.items() if k != 'gameobjects'}
        data = (b'\xef\xbb\xbf' if original.startswith(b'\xef\xbb\xbf') else b'') + result.encode('utf-8')
        planned.append((path, original, data, entry))

    snapshots = ART / 'SceneBackups'
    snapshots.mkdir(exist_ok=True)
    for path, original, data, entry in planned:
        target = snapshots / path.name
        if target.exists():
            assert target.read_bytes() == original, 'Different scene backup already exists: ' + str(target)
        else:
            target.write_bytes(original)
        sidecar = Path(str(path) + '.meta')
        if sidecar.exists():
            shutil.copy2(sidecar, snapshots / sidecar.name)

    checked_move(old, backup)
    checked_move(old.with_suffix('.meta'), backup.with_suffix('.meta'))
    checked_move(animation, backup / 'Animation')
    checked_move(animation.with_suffix('.meta'), backup / 'Animation.meta')
    checked_move(STAGE / 'Player', old)
    checked_move(STAGE / 'Player.meta', old.with_suffix('.meta'))
    checked_move(STAGE / 'Animation_Player', animation)
    checked_move(STAGE / 'Animation_Player.meta', animation.with_suffix('.meta'))
    scene_results = []
    for path, original, data, entry in planned:
        assert path.read_bytes() == original, path
        path.write_bytes(data)
        scene_results.append({'path': entry['path'], 'removed_objects': len(entry['removed']),
                              'objects_remaining': entry['objects_before']-len(entry['removed']),
                              'before_sha256': entry['sha256'], 'after_sha256': digest(path)})
    for relative, expected in audit['legacy_model_files'].items():
        assert digest(backup / relative) == expected
    for relative, expected in animation_files.items():
        assert digest(backup / 'Animation' / relative) == expected
    assert digest(backup.with_suffix('.meta')) == audit['legacy_folder_meta_sha256']
    assert digest(backup / 'Animation.meta') == animation_folder_hash
    manifest = json.loads((old / 'ExportManifest.json').read_text(encoding='utf-8'))
    manifest['status'] = 'exported_and_roundtrip_verified'
    manifest['export_directory'] = str(old)
    manifest['backup_directory'] = str(backup)
    manifest['preflight']['fbx_roundtrip_tested'] = True
    manifest['preflight']['runtime_tested'] = False
    manifest['export_execution'] = {'exported': True, 'fbx_count': 31, 'texture_count': 19,
                                  'source_actions_unchanged': True, 'engine_import_pending': True}
    manifest['controller'] = {'path': 'GreenWare/Assets/Animation/Player/Player.animcontroller',
                              'states': controller['states'], 'masks': {k: len(v) for k,v in controller['masks'].items()}}
    (old / 'ExportManifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    shutil.copy2(old / 'ExportManifest.json', ART / 'ExportManifest.json')
    for filename in ('ValidationReport.json', 'BakeReport.json', 'ControllerValidation.json'):
        shutil.copy2(STAGE / filename, ART / filename)
    with (old / 'Clips.csv').open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['Action', '表示名', 'Take', '秒数', 'Loop', 'FBX', 'RuntimeRef'])
        for clip in manifest['clips']:
            writer.writerow([clip['action'], clip['label'], clip['take'], clip['duration_seconds'], clip['loop'], clip['fbx'], clip['runtime_ref']])
    readme = '''# Player — 両手剣・54ボーン

制作元: `Assets/_src/MiniBotC/MiniBotC_PlayerMotions.blend`。2026-09-14に完成版を出力。

- `Player.fbx`: LOD0、体＋補修指。`Player_LOD1.fbx` / `Player_LOD2.fbx` は軽量版。
- `Animations/`: 承認済み22クリップ。1ファイル1テイク、30fps、120サンプル/秒。
- `Sword/`: 1本の剣、3段階LOD。`Companion/`: 4骨のおとも、3段階LOD。
- `Textures/`: 使用画像19枚を収集。FBXからの相対参照。
- `Clips.csv` / `ExportManifest.json`: 秒数・ループ・GUID参照の正本。
- `Assets/Animation/Player/Player.animcontroller`: 20ステートで全22クリップを使用。

FBZZ Editorで各FBXをインポートする。取り込み設定とループは `.fbx.meta` に設定済み。
Controllerは `Library/Baked/<FBXのGUID>/anims/<Take>.anim` の導出GUIDを参照するため、FBXのインポート後に解決される。

単位1m、上+Y。表示用VisualのY回転を180度にしてゲームの+Z前方へ合わせる。Root Motionは無効。
Swordを `Socket_Weapon_R` へ、剣側 `Attach_Grip` を基準に取り付ける。敗北時の接地も武器Socketにベイク済み。
おともの移動・浮遊はゲーム側で制御する。

旧データは隣の `Player_BackUp`。4シーンの旧Playerは削除済みで、新Playerはまだシーンへ配置していない。
再読み込みで31FBXの形状・骨格・全クリップ・武器姿勢を確認済み。FBZZ Editorでのインポート・実機再生は未確認。
詳細: `Docs/design/minibot-c-fbzz-export.md` / `Docs/design/player-export-controller.md`。
'''
    (old / 'README.md').write_text(readme, encoding='utf-8')
    animation_readme = '''# Player Controller

Base全身マスク: Player_Base（54骨）。M_UpperBody（44骨）とM_Arms（39骨）は将来の合成用。
現在は両手剣の腰・脚・武器の連動を保つため、全動作を全身で再生する。全StateのIK重みは0。

- Speed: m/s、Idle=0 / SwordWalk=2 / Run_F=6のBlendTree。
- IsGrounded: 接地、VerticalSpeed: 上向き速度m/s、IsBlocking: ガード入力。
- Jump / Dodge / Stop: 踏切・回避・走り停止Trigger。
- Slash01 / HighSpinAttack / JumpAttack / SlideAttack / UnderSlash / UnderSlashandUpperSlash: 攻撃Trigger。
- GuardHit / Hit / Death / Victory / Defeat: ガード被弾・被弾・死亡・勝利・敗北Trigger。
- Reset: Death / Victory / DefeatIdleからLocomotionへ復帰。

接続前に `Models/Player` 配下のFBXをFBZZ Editorでインポートする。
ControllerのsourcePathは原本FBXではなく、インポート後の.animを安定した導出GUIDで参照する。
デフォルトStateはLocomotion。ResultではVictoryまたはDefeatのTriggerを送る。
ゲームスクリプトの旧二刀・パリィ・登攀の処理は、このControllerでは使用しない。
'''
    (animation / 'README.md').write_text(animation_readme, encoding='utf-8')
    for path in (old / 'README.md', old / 'Clips.csv', animation / 'README.md'):
        guid_meta(path)
    report = {'export_directory': str(old), 'backup_directory': str(backup), 'legacy_files_preserved': len(audit['legacy_model_files']),
              'legacy_animation_files_preserved': len(animation_files), 'scene_changes': scene_results,
              'removed_objects_total': sum(s['removed_objects'] for s in scene_results),
              'fbx_count': 31, 'texture_count': 19, 'engine_import_pending': True}
    (ART / 'DeliveryReport.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
