"""旧Player階層と参照を列挙する。シーンの変更は行わない。"""

import hashlib
import json
import re
import subprocess
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
ASSETS = ROOT / 'GreenWare/Assets'
ART = ROOT / 'Docs/Art/MiniBotC/ExportDelivery'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_scene(path):
    text = path.read_text(encoding='utf-8-sig')
    parsed = tomllib.loads(text)
    objects = parsed.get('gameobjects', [])
    by_id = {obj['instanceId']: obj for obj in objects}
    roots = [obj['instanceId'] for obj in objects if obj.get('name') == 'Player']
    removed = set(roots)
    while True:
        previous = set(removed)
        for obj in objects:
            if obj.get('parentInstanceId') in removed:
                removed.add(obj['instanceId'])
            socket = obj.get('SocketAttachmentComponent', {})
            if socket.get('target') in removed:
                removed.add(obj['instanceId'])
        if previous == removed:
            break
    blocks = re.split(r'(?m)(?=^\[\[gameobjects\]\]\s*$)', text)
    kept, external = [blocks[0]], []
    for block in blocks[1:]:
        obj = tomllib.loads(block)['gameobjects'][0]
        if obj['instanceId'] in removed:
            continue
        for line in block.splitlines():
            if any(identity in line for identity in removed):
                external.append({'owner': obj['name'], 'owner_id': obj['instanceId'], 'line': line.strip()})
        kept.append(block)
    return {'path': path.relative_to(ROOT).as_posix(), 'sha256': digest(path),
            'objects_before': len(objects), 'roots': roots,
            'removed': [{'id': identity, 'name': by_id[identity]['name']} for identity in sorted(removed)],
            'external_references': external}


def main():
    ART.mkdir(parents=True, exist_ok=True)
    source = ASSETS / 'Models/Player'
    metas = [source.with_suffix('.meta'), *source.rglob('*.meta')]
    guids = [tomllib.loads(p.read_text(encoding='utf-8-sig'))['meta']['guid'] for p in metas]
    pattern = '|'.join(guids + [r'Models[/\\]Player', r'Animation[/\\]Player'])
    result = subprocess.run(['rg', '-n', '--hidden', '--text', '--glob', '!*.fbx', '--glob', '!*.blend*',
                             '--glob', '!*.png', '--glob', '!*.jpg', '--glob', '!*.exr',
                             pattern, str(ASSETS)], capture_output=True)
    assert result.returncode in (0, 1), result.stderr
    (ART / 'OldPlayerReferences.txt').write_bytes(result.stdout)
    scenes = [inspect_scene(path) for path in (ASSETS / 'Scenes').glob('*.scene')]
    scenes = [scene for scene in scenes if scene['roots']]
    report = {'scenes': scenes, 'legacy_model_files': {p.relative_to(source).as_posix(): digest(p) for p in source.rglob('*') if p.is_file()},
              'legacy_folder_meta_sha256': digest(source.with_suffix('.meta')), 'legacy_guids': guids}
    (ART / 'ReplacementAudit.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'scenes': [{k:v for k,v in s.items() if k != 'removed'} | {'removed_count': len(s['removed'])} for s in scenes],
                      'legacy_files': len(report['legacy_model_files'])}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
