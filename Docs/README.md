# Docs — 索引

FBZZ Engine のドキュメントは 4 か所に分かれている。**どこを読むかはこの表で決める。**

| 置き場所 | 何が書いてあるか | 読むとき |
|---|---|---|
| [`../README.md`](../README.md) | エンジンの全体像・機能一覧・ビルドとテストの手順 | リポジトリに初めて触るとき |
| [`../AGENTS.md`](../AGENTS.md) | コーディング規約・アーキテクチャ方針・禁止パターン | コードを書く前 |
| [`../CLAUDE.md`](../CLAUDE.md) | ディレクトリ地図とタスク別の入口 | 「この作業はどのファイルか」を引くとき |
| [`conventions/`](conventions/) | 守るべき運用ルール | ビルド・コミット・テストを触るとき |
| [`design/`](design/) | 機能単位の設計判断と、なぜそうしたか | 既存機能を直す / 似た機能を足すとき |
| [`../GreenWare/Assets/Docs/`](../GreenWare/Assets/Docs/) | デモゲーム GreenWare の企画書と仕様 | ゲーム側の仕様を引くとき |

ライセンス関係は [`../LICENSE`](../LICENSE) と [`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md)。

---

## conventions — 運用規約

| ファイル | 内容 |
|---|---|
| [build-performance.md](conventions/build-performance.md) | ビルド時間の規約。PCH / Unity ビルド・include を増やさないための判断基準 |
| [comments.md](conventions/comments.md) | コメント規約。Doxygen タグ・書く/書かないの基準・旧コメントの移行表・AI 向け API リファレンスの生成 |
| [git.md](conventions/git.md) | Git 運用規約。コミットの粒度とメッセージ形式・ブランチ運用 |
| [test.md](conventions/test.md) | テスト規約。Auto / Manual / Bench の切り分け・TestKit の使い方・カバレッジ設定 |

---

## design — 設計文書

機能単位の設計判断を残す場所。**「なぜこの構造か」「なぜ前の方式を捨てたか」**を書く。
コードのファイルヘッダーに書ききれない長い WHY は、ここへ置いてリンクする。

### エンジン基盤

| ファイル | 内容 |
|---|---|
| [ai-verification-loop.md](design/ai-verification-loop.md) | AI が変更の合否を自分で出す口。AgentBuild・Playtest シナリオ・絵の回帰・`--batch`・規約 lint・バスの分割 |
| [audio-system.md](design/audio-system.md) | 手続き音生成 (`.synth`) と Mixer Bus の階層 |
| [cursor.md](design/cursor.md) | カーソルの要求スタックと見た目の分離 |
| [bindless.md](design/bindless.md) | ResourceDescriptorHeap によるディスクリプタ直引き。区画割り・枠の寿命・縮退規則 |
| [dx11-removal.md](design/dx11-removal.md) | DirectX 11 サポート終了 (v1.0)。捨てた理由・残した境界・終了済み設定の扱い |
| [game-settings.md](design/game-settings.md) | ユーザー定義シリアライズと Option 画面。宣言 1 行で設定を増やす |
| [sequence-system.md](design/sequence-system.md) | 演出タイムライン (`.sequence`) |
| [sprite-reference.md](design/sprite-reference.md) | Sprite 参照の設計。名前で書き、ID で保存する契約 |

### 物理・アニメーション

| ファイル | 内容 |
|---|---|
| [active-ragdoll.md](design/active-ragdoll.md) | XPBD 関節体への移行設計 (Draft) |
| [cloth.md](design/cloth.md) | 布の CPU XPBD・ClothShader・シーン接続と段階的な導入 |
| [ragdoll-api.md](design/ragdoll-api.md) | Ragdoll の汎用 API と、ゲーム側が持つべき責務の線引き |
| [ragdoll-system-review-2026-09-13.md](design/ragdoll-system-review-2026-09-13.md) | ラグドールシステムのレビュー記録 (2026-09-13) |

### VFX・流体

| ファイル | 内容 |
|---|---|
| [vfx-prefab.md](design/vfx-prefab.md) | プレハブとしてのエフェクト (`.vfx`) |
| [fluid-editor-layout.md](design/fluid-editor-layout.md) | Fluid Editor のレイアウトとアセット選択 |
| [fluid-effect-templates.md](design/fluid-effect-templates.md) | 流体素材と、複数層を重ねる演出テンプレート |
| [volume-flipbook-baker.md](design/volume-flipbook-baker.md) | ボリュームから焼くフリップブックとモーションベクター |

### アセット制作パイプライン

| ファイル | 内容 |
|---|---|
| [blender-dcc-pipeline.md](design/blender-dcc-pipeline.md) | Blender × FBZZ の分業パイプライン。交換フォーマットと責務境界 (Draft) |
| [minibot-c-humanoid.md](design/minibot-c-humanoid.md) | MiniBot C — 人型リグと一刀モデルの制作仕様 |
| [minibot-c-player-animation-plan.md](design/minibot-c-player-animation-plan.md) | MiniBot C — Player アニメーション 22 本の実装計画 |
| [minibot-c-fbzz-export.md](design/minibot-c-fbzz-export.md) | MiniBot C — 書き出し仕様と FBZZ への受け渡し |
| [player-export-controller.md](design/player-export-controller.md) | Player 完成版の Controller 構築と旧シーンの撤去 |

### GreenWare (デモゲーム側)

ゲームの**仕様**は [`../GreenWare/Assets/Docs/`](../GreenWare/Assets/Docs/) が正本。ここに置くのは、エンジンとの境界に関わるものだけ。

| ファイル | 内容 |
|---|---|
| [greenware-script-engine-boundary.md](design/greenware-script-engine-boundary.md) | どこまでをエンジンに持たせ、どこからをスクリプトに置くか |
| [greenware-vfx-improvement-inventory.md](design/greenware-vfx-improvement-inventory.md) | エフェクトの改善・制作リスト |
| [player-camera-follow.md](design/player-camera-follow.md) | プレイヤーの画面内移動とカメラ追従 |
| [stage03-crown-island.md](design/stage03-crown-island.md) | Stage03 — 六翼の浮遊機兵と海上の孤島 |

---

## 書くときの決まり

- **設計文書は `design/` へ、規約は `conventions/` へ。** どちらでもない短い理由はコードの `/// @note` 1 行に収める ([comments.md](conventions/comments.md))
- 先頭に**状態行**を置く (`- 状態: 実装済み / 一部未着手 / Draft` など)。読む側が「これは現行の仕様か、計画か」を最初の 1 行で判別できるようにする
- 相対日付を書かない。「先週」ではなく `2026-09-13` と書く
- 取り下げた設計は**消さずに残し**、状態行へ取り下げた日と理由を書く。同じ案を作り直さないため
- `Docs/Art/` と `GreenWare/Assets/_src/` は制作時の作業記録 (レンダー画像・.blend) で、容量のため**追跡していない**。設計文書から参照するときはリンクにせず、追跡外である旨を添えてパスだけ書く
