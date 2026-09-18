# GreenWare Tools

GreenWare のアセット制作パイプライン。エンジン共通の開発ツールは `Projects/DevTools/` と `Tools/` に置く。

- `Assets/` の下には置かない (エディターが `.py` まで取り込む)
- 一回きりのスクリプトは置かない。`Scratch/` (Git に入らない) を使う
- 追加はユーザーの承認を得てから。この索引に載っていないファイルは AgentLint の `tool-unlisted` が ERROR にする

## 索引

| 名前 | 役割 | 呼び出し元 |
|------|------|-----------|
| `BlenderExport/` | MiniBot C・Boss03 などのモデリング・リグ・アニメーション・FBX 書き出し (Blender Python) | `Docs/design/minibot-c-fbzz-export.md`、`Assets/Docs/climb-core.md` |
| `SfxGen/` | 効果音の合成 (大剣・よじ登り・移動) | `Docs/design/player-combat-audio.md`、`Docs/design/movement-audio.md` |
| `VfxTextureGen/` | VFX 用テクスチャの生成 | `Assets/VFX/Textures/README.md` |

## 既知の問題

- `BlenderExport/` の約 35 本がリポジトリの旧パス `C:\Users\jinhs\Downloads\FBZZ_Engine` を直書きしており、そのままでは動かない
