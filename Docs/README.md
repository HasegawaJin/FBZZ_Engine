<!-- @file    README.md -->
<!-- @brief   FBZZ Engine の設計文書と運用規約の索引。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-09-14 -->
# Docs — 索引

更新日 **2026-11-04**。作業ツリーの CMake プロジェクト版は **0.10.0**。描画は DX12、ゲームスクリプトは C# SDK を使う。GreenWare は SDK 0.9.1 時点の旧作を置く別リポジトリ。

FBZZ Engine のドキュメントの入口を以下にまとめる。

| 置き場所 | 何が書いてあるか | 読むとき |
|---|---|---|
| [`../README.md`](../README.md) | エンジンの全体像・機能一覧・ビルドとテストの手順 | リポジトリに初めて触るとき |
| [FBZZ Engine 内部仕様](design/engine-internal-spec.md) | 依存境界、フレームの流れ、データの正本と寿命 | 実装経路や責務の分担を追うとき |
| [ランタイム最適化仕様](design/runtime-performance.md) | 各最適化の適用条件・退避条件・比較計測と採否 | 最適化の設計、実測、レビューをするとき |
| [ベンチマーク](benchmarks/README.md) | 条件別の性能比較と計測の境界 | 性能値や採用理由を確認するとき |
| [`../AGENTS.md`](../AGENTS.md) | コーディング規約・アーキテクチャ方針・禁止パターン | コードを書く前 |
| [`../CLAUDE.md`](../CLAUDE.md) | ディレクトリ地図とタスク別の入口 | 「この作業はどのファイルか」を引くとき |
| [`conventions/`](conventions/) | 守るべき運用ルール | ビルド・コミット・テストを触るとき |
| [`design/`](design/) | 機能単位の設計判断と、なぜそうしたか | 既存機能を直す / 似た機能を足すとき |
| [GreenWare 専用リポジトリ](https://github.com/HasegawaJin/GreenWare/tree/main/Assets/Docs) | デモゲーム GreenWare の企画書と仕様 | ゲーム側の仕様を引くとき |

ライセンス関係は [`../LICENSE`](../LICENSE) と [`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md)。

## 現行構成と最近の変更

| 項目 | 現在の状態・参照先 |
|---|---|
| Core / Graphics | `Projects/Core` と `Projects/Graphics` を独立 DLL に分割済み。Graphics の公開依存は Core / Math。Engine / Physics / Fluid へ依存しない ([設計と検証](design/graphics-library.md)) |
| 描画入力 | メッシュ・スキニングに加え、ライト・環境・各エフェクト固有入力を Engine で抽出し、`RenderScene` の値と非所有 GPU ハンドルで Graphics へ渡す |
| Forward / Deferred | Graphics 内のパイプライン構成として分離。材質ごとの経路と共通処理は [graphics-library.md](design/graphics-library.md)、導入背景は [pipeline-boundary.md](design/pipeline-boundary.md) |
| SDK | `SDK/0.10.0/` に Core / Graphics と managed starter のヘッダー・ライブラリ・DLL を配布。`lib/Development/FBZZGraphics.lib` はインポートライブラリで、実行時には `bin/Development/FBZZGraphics.dll` も必要。SDK 0.9.1 は GreenWare 用の旧版 |
| 物理 | BroadPhase の BVH を保持し、変更した葉と祖先を refit。総当たりの候補集合と比較し、静止・移動・再構築の費用を別々に計測した。 [差分更新](design/broadphase-incremental.md) · [計測](benchmarks/2026-10-22-broadphase-incremental/report.md) |
| 描画の選択経路 | 多数灯の重要度サンプリング、GPU 間接描画／Mesh Shader、影候補の BVH を条件付きで選択・比較できる。DXR／Path Tracing は DeveloperMode の実験。 [多数灯](design/many-light-sampling.md) · [GPU 駆動](design/gpu-driven-geometry.md) · [空間索引](design/spatial-water-optimization.md) · [RT 実験](design/rt-experiment.md) |
| C# とシーン | Play 用 Scene・managed 世代を分離し、型付き SDK と共通 schema を使用。コンポーネントの一括処理・寿命管理は既存の階層と組み合わせる。 [Play と設定](design/csharp-settings-transactions.md) · [一括処理](design/ecs-bulk-processing.md) |
| 描画検証の記録 | 2026-09-21 の Graphics 分割で関連 369 テストと DX12 単独描画を検証。FiberLifecycle は当時の基準画像更新後、93 手順合格・10 枚の画素差が 0。 [分割の検証](design/graphics-library.md) · [Fiber](design/fiber-rendering.md) |

---

## 性能の比較記録

| 計測 | 比較した範囲 |
|---|---|
| [BroadPhase の差分更新](benchmarks/2026-10-22-broadphase-incremental/report.md) | 旧方式と保持 BVH。静止、少数移動、再構築、World 同期込みの費用 |
| [描画同期](benchmarks/2026-10-22-render-synchronization/report.md) | CPU 記録・描画完了時間を分け、実 DX12 と fake backend、ビュー数別に比較 |
| [静的メッシュの再利用](benchmarks/2026-10-23-static-mesh-reuse/Report.md) | CPU 抽出と DX12 実描画。変更率ごとの利得と悪化 |
| [可視メッシュの尾部遅延](benchmarks/2026-10-23-visible-mesh-tail/Report.md) | GPU パス時間とフレーム時間、長いフレームの内訳 |
| [Math のベンチマーク](benchmarks/README.md) | インライン化・SIMD の自動生成索引と生データへの入口 |

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
| [profiler.md](design/profiler.md) | Performance / Script Profiler と Memory Debug の分割、計測と履歴、障害復旧、既存 API の移行契約 (Draft) |
| [developer-mode.md](design/developer-mode.md) | 開発者モードの設定と機能 |
| [gamehub-update-notice.md](design/gamehub-update-notice.md) | GameHub の新しい版の通知 |
| [csharp-start.md](csharp-start.md) | ソースなしSDKからのC#導入、GameHub、共通雛形・コンパイル例、AIの入口 |
| [csharp-only-gameplay.md](design/csharp-only-gameplay.md) | C# ゲームスクリプトと C++ Engine の責務境界 |
| [csharp-scripting.md](design/csharp-scripting.md) | C# API境界・参照・保存・差し替えと配布の現行契約 |
| [csharp-authoring.md](design/csharp-authoring.md) | protected override lifecycle、属性ゼロの基本設定、公開API・単位・座標・階層・配列の契約 |
| [ai-script-workflow.md](design/ai-script-workflow.md) | Script の宣言から共通 schema、AI の追加・設定・参照接続、保存・Playtest へ進む制作経路 (Draft) |
| [scene-capacity.md](design/scene-capacity.md) | Entity 上限での安全な作成失敗と複数生成の容量確認 |
| [ecs-bulk-processing.md](design/ecs-bulk-processing.md) | コンポーネント格納、生成・同期・破棄の一括処理 |
| [csharp-settings-transactions.md](design/csharp-settings-transactions.md) | Edit／Play の分離と世代付き設定の適用 |
| [benchmark-report.md](design/benchmark-report.md) | ベンチマークの計測と比較レポート |
| [game-settings.md](design/game-settings.md) | ユーザー定義シリアライズと Option 画面。宣言 1 行で設定を増やす |
| [sequence-system.md](design/sequence-system.md) | 演出タイムライン (`.sequence`) |
| [sprite-reference.md](design/sprite-reference.md) | Sprite 参照の設計。名前で書き、ID で保存する契約 |

### Graphics・描画

| ファイル | 内容 |
|---|---|
| [dx12-agility-sdk.md](design/dx12-agility-sdk.md) | Agility SDK の EXE 指定、Graphics の非公開依存、SDK とゲーム配布、起動診断と受入条件 |
| [shared-engine-sdk.md](design/shared-engine-sdk.md) | SDK を正本にした Editor 起動、同構成の更新・公開、DLL 同期の契約 |
| [bindless.md](design/bindless.md) | ResourceDescriptorHeap によるディスクリプタ直引き。区画割り・枠の寿命・縮退規則 |
| [dx11-removal.md](design/dx11-removal.md) | DirectX 11 の撤去理由・残した境界・旧設定の扱い。文書中の v1.0 表記は導入時の記録 |
| [graphics-library.md](design/graphics-library.md) | Core / Graphics の物理分割、RenderScene 全入力の抽出、Forward / Deferred の構成境界、SDK と単独描画の検証。§8.2 が今回の実装範囲 |
| [pipeline-boundary.md](design/pipeline-boundary.md) | 材質ごとの GBuffer / Forward 振り分け規則とスキンド描画の導入背景 |
| [render-graph.md](design/render-graph.md) | RenderGraph の責務と資源・パスの依存関係 |
| [render-pass-viewer.md](design/render-pass-viewer.md) | Render Pass Viewer による描画パスの確認 |
| [pix-profiling.md](design/pix-profiling.md) | PIX の起動接続、イベント、Release キャプチャと性能検証の契約 |
| [rt-experiment.md](design/rt-experiment.md) | DeveloperMode に保持する RT 実験、公開用 Reference 画像、撮影条件・保留理由・再開条件 |
| [async-compute.md](design/async-compute.md) | 非同期コンピュートキュー |
| [render-thread.md](design/render-thread.md) | フレーム入力の確定、描画スレッドの所有、同期と読み戻し |
| [gpu-driven-geometry.md](design/gpu-driven-geometry.md) | CPU、間接描画、Mesh Shader の適用範囲と比較 |
| [many-light-sampling.md](design/many-light-sampling.md) | Froxel の候補と画素側のサンプリング、画質・性能の比較 |
| [spatial-water-optimization.md](design/spatial-water-optimization.md) | 影候補 BVH、差分更新の比較、波紋と Upload Arena |
| [gpu-instancing.md](design/gpu-instancing.md) | 同じメッシュの自動インスタンシング |
| [view-culling.md](design/view-culling.md) | ビューごとのカリング結果の共有 |
| [shader-reload-lifetime.md](design/shader-reload-lifetime.md) | シェーダー再読み込みと描画キャッシュの寿命 |
| [shader-capabilities.md](design/shader-capabilities.md) | 名前に依存しない表面・頂点・被覆・GBuffer 変種とプレビューの宣言 |
| [light-probe-gi.md](design/light-probe-gi.md) | Light Probe Volume のベイク・GI と比較シナリオ |
| [fiber-rendering.md](design/fiber-rendering.md) | 毛・草の Shell / Fin / Hybrid / Blade、スキニング・風・接触、Lifecycle 画像回帰 |
| [terrain-layers.md](design/terrain-layers.md) | 地形の可変レイヤー・質感・穴・ブラシ |
| [water-waves.md](design/water-waves.md) | 水面の波の周波数帯と接続 |
| [water-foundation.md](design/water-foundation.md) | 水域照会・座標・物理と描画品質の分離 |
| [water-surface-quality.md](design/water-surface-quality.md) | 白波・遠景海面・Terrain の岸泡と水深情報 |
| [water-wave-optics-morph.md](design/water-wave-optics-morph.md) | 波アセット・浅水減衰・汎用LODモーフ・吸収と散乱 |
| [water-spline-surface.md](design/water-spline-surface.md) | 湖と川のスプライン形状・水深・流速を描画と物理へ共有 |
| [water-zone.md](design/water-zone.md) | 接続水域の共通メッシュ・重なりの優先順位・河口の共有属性 |
| [control-point-authoring.md](design/control-point-authoring.md) | 共通TargetPoint・用途別Binding・Gizmo/Undo・複製・数値snapshotと静的ベイクの設計 |
| [control-point-usage-audit.md](design/control-point-usage-audit.md) | Camera・IK・VFX・AI・NavLink等のTargetPoint利用箇所と追加adapterの優先順位 |
| [surface-patch-lod.md](design/surface-patch-lod.md) | 連続面の汎用パッチ LOD・共有格子・境界の縫合 |
| [water-flow-optimization.md](design/water-flow-optimization.md) | 水面評価と速度場 PNG |

### 物理・アニメーション

| ファイル | 内容 |
|---|---|
| [active-ragdoll.md](design/active-ragdoll.md) | XPBD 関節体への移行設計 (Draft) |
| [broadphase-incremental.md](design/broadphase-incremental.md) | 保持 BVH の refit・再構築条件・総当たり比較 |
| [ragdoll-controls.md](design/ragdoll-controls.md) | XPBD 関節体の部位制御と接触・支持の契約 |
| [cloth.md](design/cloth.md) | 布の CPU XPBD・ClothShader・シーン接続と段階的な導入 |
| [buoyancy.md](design/buoyancy.md) | 浮力の法則と、水面・コライダーとの責務境界 |
| [math-simd.md](design/math-simd.md) | 自作 Math の SIMD 化と検証 |
| [ragdoll-api.md](design/ragdoll-api.md) | Ragdoll の汎用 API と、ゲーム側が持つべき責務の線引き |
| [ragdoll-system-review-2026-09-13.md](design/ragdoll-system-review-2026-09-13.md) | ラグドールシステムのレビュー記録 (2026-09-13) |

### VFX・流体

| ファイル | 内容 |
|---|---|
| [vfx-prefab.md](design/vfx-prefab.md) | プレハブとしてのエフェクト (`.vfx`) |
| [prefab-safety.md](design/prefab-safety.md) | GUID による出所・適用済み定義との差分・検証後の階層差し替え |
| [script-transform-contract.md](design/script-transform-contract.md) | スクリプトのローカル / ワールド座標の書き込みと即時同期 |
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
| [geometry-authoring.md](design/geometry-authoring.md) | 独立 AssetEditor と Terrain / Mesh / Paint 共通基盤の検討記録。費用対効果により実装見送り |
| [asset-streaming.md](design/asset-streaming.md) | アセットの非同期読み込み・ストリーミングと段階ごとの実装範囲 |
| [material-reflection-types.md](design/material-reflection-types.md) | Material Reflection の型契約 |
| [minibot-c-humanoid.md](design/minibot-c-humanoid.md) | MiniBot C — 人型リグと一刀モデルの制作仕様 |
| [minibot-c-player-animation-plan.md](design/minibot-c-player-animation-plan.md) | MiniBot C — Player アニメーション 22 本の実装計画 |
| [minibot-c-fbzz-export.md](design/minibot-c-fbzz-export.md) | MiniBot C — 書き出し仕様と FBZZ への受け渡し |
| [player-export-controller.md](design/player-export-controller.md) | Player 完成版の Controller 構築と旧シーンの撤去 |

### GreenWare (SDK 0.9.1 の旧作資料)

GreenWare は SDK 0.9.1 で作られた旧作で、C++ ゲーム Script の資料・測定は現行 SDK の使用例ではない。ゲームの**仕様**は[専用リポジトリ](https://github.com/HasegawaJin/GreenWare/tree/main/Assets/Docs)が正本。

この索引以降に残る GreenWare 固有のパス・実行記録は、別記がなければ専用リポジトリのルートからの相対パスであり、SDK 0.9.1 時点の履歴を表す。

| ファイル | 内容 |
|---|---|
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
- `Docs/Art/` は制作時の作業記録 (レンダー画像・.blend) で、容量のため**追跡していない**。GreenWare の作業ファイルは[専用リポジトリ](https://github.com/HasegawaJin/GreenWare)側で管理する
