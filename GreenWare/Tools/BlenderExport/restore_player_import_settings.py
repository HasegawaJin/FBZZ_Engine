"""自動取り込みで既定値に戻った新Playerの設定を、既存GUIDを保ってManifestへ揃える。"""

import json
import re
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PACKAGE = ROOT / 'GreenWare/Assets/Models/Player'


def main():
    manifest = json.loads((PACKAGE / 'ExportManifest.json').read_text(encoding='utf-8'))
    clips = {c['take']: c for c in manifest['clips']}
    changed = []
    for path in PACKAGE.rglob('*.fbx.meta'):
        original = path.read_text(encoding='utf-8')
        parsed = tomllib.loads(original)
        result = re.sub(r"(?m)^source_dcc\s*=.*$", "source_dcc = 'Blender'", original)
        result = re.sub(r"(?m)^normal_map_convention\s*=.*$", "normal_map_convention = 'OpenGL'", result)
        if path.parent.name == 'Animations':
            clip = clips[path.name.removesuffix('.fbx.meta')]
            assert parsed['meta']['guid'] == clip['source_guid']
            if not parsed['model'].get('clips'):
                label = clip['take']
                settings = f"clips = [{{ name = '{label}', loop = {str(clip['loop']).lower()}, start_frame = 0.0, end_frame = -1.0, output_name = '{label}' }}]"
                result, count = re.subn(r'(?m)^clips\s*=\s*\[\s*\]\s*$', settings, result)
                assert count == 1
            else:
                assert parsed['model']['clips'][0]['loop'] == clip['loop']
        assert tomllib.loads(result)['meta']['guid'] == parsed['meta']['guid']
        if result != original:
            path.write_text(result, encoding='utf-8')
            changed.append(path.relative_to(PACKAGE).as_posix())
    print(json.dumps({'updated_settings': len(changed), 'guids_preserved': True}))


if __name__ == '__main__':
    main()
