# Docs — 索引

更新日: 2026-09-21。対象は現行作業ツリー (`CMakeLists.txt` のプロジェクト版 `0.9.1`)。リリース済みかどうかと、各機能の実装・検証状況は区別し、設計文書の状態行と検証記録を参照する。

FBZZ Engine のドキュメントの入口を以下にまとめる。

| 置き場所 | 何が書いてあるか | 読むとき |
|---|---|---|
| [`../README.md`](../README.md) | エンジンの全体像・機能一覧・ビルドとテストの手順 | リポジトリに初めて触るとき |
| [`../AGENTS.md`](../AGENTS.md) | コーディング規約・アーキテクチャ方針・禁止パターン | コードを書く前 |
| [`../CLAUDE.md`](../CLAUDE.md) | ディレクトリ地図とタスク別の入口 | 「この作業はどのファイルか」を引くとき |
| [`conventions/`](conventions/) | 守るべき運用ルール | ビルド・コミット・テストを触るとき |
| [`design/`](design/) | 機能単位の設計判断と、なぜそうしたか | 既存機能を直す / 似た機能を足すとき |
| [`../GreenWare/Assets/Docs/`](../GreenWare/Assets/Docs/) | デモゲーム GreenWare の企画書と仕様 | ゲーム側の仕様を引くとき |

ライセンス関係は [`../LICENSE`](../LICENSE) と [`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md)。

## 現行構成と最近の変更

| 項目 | 現在の状態・参照先 |
|---|---|
| Core / Graphics | `Projects/Core` と `Projects/Graphics` を独立 DLL に分割済み。Graphics の公開依存は Core / Math。Engine / Physics / Fluid へ依存しない ([設計と検証](design/graphics-library.md)) |
| 描画入力 | メッシュ・スキニングに加え、ライト・環境・各エフェクト固有入力を Engine で抽出し、`RenderScene` の値と非所有 GPU ハンドルで Graphics へ渡す |
| Forward / Deferred | Graphics 内のパイプライン構成として分離。材質ごとの経路と共通処理は [graphics-library.md](design/graphics-library.md)、導入背景は [pipeline-boundary.md](design/pipeline-boundary.md) |
| SDK | `SDK/0.9.1/` に Core / Graphics のヘッダー・ライブラリ・DLL を配布。`lib/Development/FBZZGraphics.lib` はインポートライブラリで、実行時には `bin/Development/FBZZGraphics.dll` も必要 |
| 描画検証 | 関連 369 テストと Graphics 単独の DX12 描画を検証。FiberLifecycle は基準画像更新後、93 手順合格・10 枚すべて画素差 0 ([記録](design/fiber-rendering.md)) |

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
コードのコメントに収まらない設計理由は、ここへ置いてリンクする。

### エンジン基盤

| ファイル | 内容 |
|---|---|
| [ai-verification-loop.md](design/ai-verification-loop.md) | AI が変更の合否を自分で出す口。AgentBuild・Playtest シナリオ・絵の回帰・`--batch`・規約 lint・バスの分割 |
| [audio-system.md](design/audio-system.md) | 手続き音生成 (`.synth`) と Mixer Bus の階層 |
| [cursor.md](design/cursor.md) | カーソルの要求スタックと見た目の分離 |
| [crash-report.md](design/crash-report.md) | クラッシュ情報の収集と報告 |
| [developer-mode.md](design/developer-mode.md) | 開発者モードの設定と機能 |
| [gamehub-update-notice.md](design/gamehub-update-notice.md) | GameHub の新しい版の通知 |
| [script-dll-recovery.md](design/script-dll-recovery.md) | Scripts DLL の読み込み失敗と復帰 |
| [benchmark-report.md](design/benchmark-report.md) | ベンチマークの計測と比較レポート |
| [game-settings.md](design/game-settings.md) | ユーザー定義シリアライズと Option 画面。宣言 1 行で設定を増やす |
| [sequence-system.md](design/sequence-system.md) | 演出タイムライン (`.sequence`) |
| [sprite-reference.md](design/sprite-reference.md) | Sprite 参照の設計。名前で書き、ID で保存する契約 |

### Graphics・描画

| ファイル | 内容 |
|---|---|
| [bindless.md](design/bindless.md) | ResourceDescriptorHeap によるディスクリプタ直引き。区画割り・枠の寿命・縮退規則 |
| [dx11-removal.md](design/dx11-removal.md) | DirectX 11 の撤去理由・残した境界・旧設定の扱い。文書中の v1.0 表記は導入時の記録 |
| [graphics-library.md](design/graphics-library.md) | Core / Graphics の物理分割、RenderScene 全入力の抽出、Forward / Deferred の構成境界、SDK と単独描画の検証。§8.2 が今回の実装範囲 |
| [pipeline-boundary.md](design/pipeline-boundary.md) | 材質ごとの GBuffer / Forward 振り分け規則とスキンド描画の導入背景 |
| [render-graph.md](design/render-graph.md) | RenderGraph の責務と資源・パスの依存関係 |
| [render-pass-viewer.md](design/render-pass-viewer.md) | Render Pass Viewer による描画パスの確認 |
| [pix-profiling.md](design/pix-profiling.md) | PIX の起動接続、イベント、Release キャプチャと性能検証の契約 |
| [async-compute.md](design/async-compute.md) | 非同期コンピュートキュー |
| [gpu-instancing.md](design/gpu-instancing.md) | 同じメッシュの自動インスタンシング |
| [view-culling.md](design/view-culling.md) | ビューごとのカリング結果の共有 |
| [shader-reload-lifetime.md](design/shader-reload-lifetime.md) | シェーダー再読み込みと描画キャッシュの寿命 |
| [light-probe-gi.md](design/light-probe-gi.md) | Light Probe Volume のベイク・GI と比較シナリオ |
| [fiber-rendering.md](design/fiber-rendering.md) | 毛・草の Shell / Fin / Hybrid / Blade、スキニング・風・接触、Lifecycle 画像回帰 |
| [terrain-layers.md](design/terrain-layers.md) | 地形の可変レイヤー・質感・穴・ブラシ |
| [water-waves.md](design/water-waves.md) | 水面の波の周波数帯と接続 |
| [water-flow-optimization.md](design/water-flow-optimization.md) | 水面評価と速度場 PNG |

### 物理・アニメーション

| ファイル | 内容 |
|---|---|
| [active-ragdoll.md](design/active-ragdoll.md) | XPBD 関節体への移行設計 (Draft) |
| [cloth.md](design/cloth.md) | 布の CPU XPBD・ClothShader・シーン接続と段階的な導入 |
| [buoyancy.md](design/buoyancy.md) | 浮力の法則と、水面・コライダーとの責務境界 |
| [math-simd.md](design/math-simd.md) | 自作 Math の SIMD 化と検証 |
| [ragdoll-api.md](design/ragdoll-api.md) | Ragdoll の汎用 API と、ゲーム側が持つべき責務の線引き |
| [ragdoll-system-review-2026-09-13.md](design/ragdoll-system-review-2026-09-13.md) | ラグドールシステムのレビュー記録 (2026-09-13) |

### VFX・流体

| ファイル | 内容 |
|---|---|
| [vfx-prefab.md](design/vfx-prefab.md) | プレハブとしてのエフェクト (`.vfx`) |
| [fluid-library.md](design/fluid-library.md) | Fluid の数式ライブラリと Engine 側の結合・実行の分離 |
| [fluid-determinism.md](design/fluid-determinism.md) | 流体シミュレーションと素材生成の決定論 |
| [flow-field.md](design/flow-field.md) | 媒質の速度を表す流れの場 |
| [fluid-editor-layout.md](design/fluid-editor-layout.md) | Fluid Editor のレイアウトとアセット選択 |
| [fluid-effect-templates.md](design/fluid-effect-templates.md) | 流体素材と、複数層を重ねる演出テンプレート |
| [volume-flipbook-baker.md](design/volume-flipbook-baker.md) | ボリュームから焼くフリップブックとモーションベクター |

### アセット制作パイプライン

| ファイル | 内容 |
|---|---|
| [blender-dcc-pipeline.md](design/blender-dcc-pipeline.md) | Blender × FBZZ の分業パイプライン。交換フォーマットと責務境界 (Draft) |
| [asset-streaming.md](design/asset-streaming.md) | アセットの非同期読み込み・ストリーミングと段階ごとの実装範囲 |
| [material-reflection-types.md](design/material-reflection-types.md) | Material Reflection の型契約 |
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
| [boss-entrance.md](design/boss-entrance.md) | ボス登場演出と Stage02 開幕配置 |
| [boss03-counter-opening.md](design/boss03-counter-opening.md) | Boss03 の羽の反射・落下・反撃機会 |
| [boss03-ground-fire-and-scoring.md](design/boss03-ground-fire-and-scoring.md) | Boss03 の残り火とステージ別採点 |
| [greenware-boss-polish.md](design/greenware-boss-polish.md) | 全ボスの攻防調整 |
| [parry-rush.md](design/parry-rush.md) | パリィ連撃チャンス |
| [movement-audio.md](design/movement-audio.md) | Player / Boss の移動 SE |
| [player-combat-audio.md](design/player-combat-audio.md) | Player の戦闘 SE |
| [player-danger-and-result.md](design/player-danger-and-result.md) | Player の危機・死亡・Result 演出 |

---

## 書くときの決まり

- **設計文書は `design/` へ、規約は `conventions/` へ。** どちらでもない短い理由はコードの `/// @note` 1 行に収める ([comments.md](conventions/comments.md))
- 先頭に**状態行**を置く (`- 状態: 実装済み / 一部未着手 / Draft` など)。読む側が「これは現行の仕様か、計画か」を最初の 1 行で判別できるようにする
- 相対日付を書かない。「先週」ではなく `2026-09-13` と書く
- 取り下げた設計は**消さずに残し**、状態行へ取り下げた日と理由を書く。同じ案を作り直さないため
- `Docs/Art/` と `GreenWare/Assets/_src/` は制作時の作業記録 (レンダー画像・.blend) で、容量のため**追跡していない**。設計文書から参照するときはリンクにせず、追跡外である旨を添えてパスだけ書く
