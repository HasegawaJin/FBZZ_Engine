<!-- @file    EditorDirectApi.md
@brief   画面表示に依存しないエディター操作の共通契約。
@author  Hasegawa Jin
@date    2026-11-04 -->
# Editor の直接 API

操作の正本は `Editor/src/Op/` の `OperatorRegistry` とする。MCP は既存の
`editor_op_list`、`editor_op_query`、`editor_op_invoke` を使う。画面を開く要求や
座標指定のクリックを、設定変更・変換・保存の代わりに使わない。

`editor_op_list` の `params`、`kind`、実行可否を最初に確認する。`query` は照会、
`action` は作業状態やファイル操作、`mutation` は共通 Undo に載る編集である。
設定の入れ子は各 `inspect` の結果を正本にし、フィールド名を推測しない。

## 操作群

| ID | 契約 |
|---|---|
| `asset.model.inspect/configure/import/status` | FBX・glTF・GLB・OBJ 原本の内容、変換設定、変換ジョブ、生成物の参照を扱う。 |
| `asset.texture.inspect/configure/reset/import/trim` | テクスチャの全インポート設定と Sprite 矩形を扱う。型の変更でも既存の矩形と ID を保持する。 |
| `asset.creation_catalog`、`asset.create` | 種類・拡張子・プリセットを照会し、通常の Create と同じ既定内容を生成する。 |
| `asset.move/rename/duplicate/extract/trash` | 本体と meta をまとめて扱う。移動は GUID を維持し、複写は再発行する。既存の出力は上書きしない。 |
| `asset.guid.inspect/repair` | Asset Maintenance と同じグループと修復規則を使う。最古の meta を保持し、生成物と import fingerprint も追従させる。 |
| `asset.references.scan/status` | Find References と同じ保存済み TOML の直接参照を、根拠のフィールド・行番号付きで返す。 |
| `project.settings.inspect/configure/reset/reload/save` | プロジェクト設定を部分編集する。編集・初期化・再読込は共通 Undo に載る。 |
| `input.bindings.inspect/configure/reset/reload/save/rebind/cancel_rebind/preview` | 入力設定の編集・保存とリバインドを扱う。編集・初期化・再読込は共通 Undo に載る。 |
| `workspace.settings.inspect/configure/reload/save` | エディター設定、ホットキー、パネル設定をライブ状態へ反映する。 |
| `workspace.layout.inspect/configure/save` | ドッキング配置の INI 本文を取得・適用・保存する。 |
| `sequence.inspect/open/configure/save/reload/close/undo/redo/control` | Timeline の作業ドキュメントとパネル内履歴を共有し、非表示でも編集・プレビューできる。 |
| `animation.preview.inspect/open/control/analyze/capture/status` | 単体クリップ・遷移のプレビュー、再生位置、ルートモーション解析、PNG 撮影を扱う。 |
| `ibl.inspect/configure/bake` | HDRI ベイクの全設定と要求・完了結果を扱う。 |
| `renderpass.inspect/configure/capture` | パネル非表示でも描画パス・資源・途中画像を捕捉できる。 |
| `volume.flipbook.inspect/configure/reset_look/control/bake/cancel/apply/preview_vfx` | 解析 puff の全設定・再生・比較・ベイク・材質適用をパネルと共有する。 |
| `fluid.editor.inspect/open/configure/save/save_as/reload/keep_local/close/control/part/bake` | 未保存レシピ・履歴・競合・再生位置・部品表示を Fluid Editor と共有する。 |
| `vfx.timeline.inspect/control` | トラック一覧と非表示で維持されるスクラブを扱う。 |
| `map.grid.inspect/create/configure/cell/fill` | グリッドとセルの生成・割当解除・選択・フォーカスを扱う。寸法変更でも座標を維持する。 |
| `console.inspect/select/clear` | 検索・Collapse を適用した表示行、安定した通し番号による選択とコピー内容、消去を扱う。 |
| `profiler.{performance,script,memory}.history/history_control` | 同じ保持フレームと表示選択を使い、固定・再開・取得・選択・消去する。 |
| `profiler.memory.set_recording/baseline/diff` | メモリ履歴の記録と Renderer リソース台帳の基準・増減・Play/Stop 結果を扱う。 |
| `profiler.performance.render_plan/save_render_plan` | 最新または明示した保持フレームの描画計画を取得・保存する。 |
| `component.collider.rebuild` | Renderer またはモデルの CPU メッシュから再構築する。失敗時は古い形状を保持する。 |
| `component.light_probe.inspect/bake` | プローブのベイク要求と要求後の進捗を扱う。 |
| `component.animator.create_controller` | 現在の状態から Controller を作り、GUID で接続する。Undo は接続を戻す。 |
| `asset.hlsl.create` | プロジェクトの HLSL テンプレート生成をメニューと共有する。 |
| `asset.vector_field.inspect/bake` | 速度場の内容照会と手続きベイクを扱う。 |
| `asset.flipbook.scan_sequence/bake_atlas/generate` | 連番を番号順に読み、アトラスと手続き画像を生成する。材質を指定すると Particle 描画パスに切り替え、画像と分割設定を保存する。 |
| `edit.history.inspect/control` | 共通 Undo 履歴の照会・移動・消去を扱う。 |
| `ai.setup.inspect/registration_command/register_desktop/launch_desktop` | 接続診断とクライアント設定を共通サービスから実行する。 |
| `build.package_reset/package_launch` | 配布結果のリセットと、完了済みの配布ゲームの起動を扱う。 |
| `material.preview.inspect/configure/reset`、`inspector.material.preview.inspect/configure/reset` | 2 つの材質ビューを個別に設定する。形状、チャンネル、照明、背景、露出、視点、アニメーション、Fiber 表示を扱う。 |
| `build.output.inspect/control` | ビルド履歴、選択中の診断、最新の生ログを返す。履歴の選択・消去、最新追従、先頭エラーへの移動、診断欄の比率を扱う。 |
| `component.ui_image.set_native_size` | Inspector と同じ原寸解決を使い、ストレッチしていない軸を変更して Undo に記録する。 |
| `search.everything/refresh/focus_object` | Search Everything と同じ索引でシーンとアセットを横断検索し、再索引、オブジェクトの選択とカメラ移動を行う。 |
| `build.settings.inspect/configure` | 配布パネルの設定を共有する。シーンの追加・削除・有効化・順序変更を一覧として保存できる。 |
| `animation.graph.view.inspect/control`、`behavior_tree.view.inspect/control` | ズーム、パン、リセット、全体表示と選択範囲表示を扱う。 |
| `hierarchy.view.inspect/configure` | 検索・マップ絞り込みと、オブジェクト指定による枝の開閉を扱う。 |
| `sprite.editor.inspect/open/configure/control` | 作業コピー、矩形の編集、選択、パン・ズーム、全体表示、適用と破棄を扱う。 |
| `asset.browser.{1..4}.inspect/configure` | ブラウザーごとの検索、型フィルター、フォルダー横断検索、Grid/Listを扱う。 |

## 画面別の照合

画面の生成元は `EditorApp` の `m_panels` とする。アセットプレビューと
補助画面も含め、現在の操作経路を下表に示す。

| 画面 | 直接 API の経路 |
|---|---|
| Scene Hierarchy | ノード作成・移動・削除、選択、Prefab 操作、検索・絞り込み・枝の開閉 |
| Inspector | コンポーネント照会・変更・並べ替え、原寸、材質プレビュー、`workspace.settings` のセクション表示状態 |
| Preview | Animation と Material のプレビュー操作、アイコンの表示 |
| Animation Mask Preview | マスクのアセット操作、`animation.mask.preview.*` の対象・視点・骨選択 |
| Animation Graph | グラフ、状態、遷移、条件、モーション、レイヤー、パラメータ、表示位置・拡縮 |
| Behavior Tree | ノード、リンク、ブラックボードの編集、表示位置・拡縮 |
| SFX Editor | `sfx.*` の合成・編集 |
| Sequence | `sequence.*` の文書・再生・保存、`sequence.curve.*` の選択と表示範囲 |
| Fluid Editor | `fluid.editor.*` の文書・再生・部品・ベイク |
| VFX Timeline | `vfx.timeline.*` とコンポーネント編集 |
| Sprite Editor | テクスチャ設定、スライス、リネーム、トリム、作業コピーと表示操作 |
| Scene / Game / UI Viewport | カメラ、撮影、レンダー設定、ギズモ、`viewport.overlays.*`、Game Viewの入力状態と制御 |
| Console | 行・選択・消去 |
| Script Requirements | `script.requirements.validate/reveal` |
| Build Output | `build.output.*`、ビルド実行・状態、ログフィルター、診断行のソースを開く |
| Asset Browser | ファイル・インポート・GUID・参照検索、参照先への移動、各ブラウザーの絞り込み・表示モード |
| Undo History | `edit.history.*` |
| Hotkeys | ワークスペース設定のキー割当、`hotkey.capture.*` の待受・競合表示 |
| Project Settings | `project.settings.*`、`input.bindings.*` |
| Build Settings | 配布の検査・実行・状態・停止・起動、シーン一覧と設定 |
| Script Profiler | 計測履歴・固定・取得・選択・消去 |
| Memory Debug | 履歴・記録・基準・差分、`profiler.memory.view.*` のタグと軸範囲 |
| Performance Profiler | 履歴・描画計画・出力、`profiler.performance.view.*` の検出閾値とカテゴリ表示 |
| Render Pass Viewer | パス照会・設定・捕捉 |
| Map Editor | 編集モード・グリッド・セル選択とフォーカス・`map.grid.cell(action=create)` の地形追加・`workspace.settings` の `terrain_tool` ブラシ設定 |
| Dependency View | 保存済み参照のスキャン、`asset.reveal(select=true)` で参照元へ移動 |
| IBL Bake | `ibl.*` |
| Volume Flipbook Bake | `volume.flipbook.*` |
| Navigation | ベイク・可視化・コンポーネント設定 |
| Asset Maintenance | GUID の照会・修復 |
| AI Settings | Command Bus、接続診断、登録コマンド・クライアント起動 |
| Search Everything | 横断検索、再索引、アセットを開く、オブジェクト選択と視点移動 |
| Status Bar / 通知 | ビルド状態、Play 制御、先頭エラーへの移動、通知の表示状態・消去 |

## 部分編集と保存

設定オブジェクトは指定キーだけを変更し、配列は全体を置換する。未知キー、型不一致、
受理後に別の値になる範囲外入力は、ライブ状態・ファイル・履歴を変更する前に拒否する。
モデル・テクスチャの enum は `inspect` が返す文字列、設定ドキュメントは通常保存のキーを使う。

Project Settings と入力は、ファイルの読込から値の解析を分離する。プロジェクト設定だけの
編集・Undo が `Input.inputactions` を再読込して入力を置き換えることはない。
Undo/Redo は対象プロジェクトと期待する現在値を検査し、外から変更された値を上書きしない。

`project.settings.save` は保存要求を受け付ける。`inspect.data.saveState` が `saved` に
なるまで確認する。エディター設定は通常と同じ共有ファイルと個人状態へ振り分ける。
Sequence の履歴はドキュメント固有なので `sequence.undo/redo` を使う。未保存文書を
閉じる・読み直す・切り替える操作は、保存または明示した `discard=true` を必要とする。

ファイルの生成・移動・ごみ箱への退避はシーンの Undo と分離する。未保存アセットは先に
保存する。`asset.trash` は `.fbzz/Trash` の退避先を返し、元の meta も保持する。

## 変換・撮影の完了

モデル変換は `asset.model.import` が返す `job` を `asset.model.status` へ渡す。
`state=done` と `imported/optionsSaved/cacheSaved` を確認し、`outputs` の `reference` を
アニメーター等へ接続する。原本 FBX を `.anim` として接続しない。UI と MCP の変換は同じ
処理で直列化し、変換中の meta 設定確定と別の設定保存を競合させない。
アニメーションの再変換は一時ファイルから公開する。削除共有で開いた読み手は既存の内容を
引き続き読める。再変換の失敗時に既存の出力ディレクトリを一括削除しない。

`animation.preview.capture` はジョブを受け付け、次の描画フレームでプレビューを記録する。
提出後の次の更新で PNG を読み戻し、`animation.preview.status` の `state=done` と
`path/width/height` を返す。DX12 の描画記録はフレーム内、画像読戻しはフレーム外で行う。
失敗は `state=failed` と理由を返し、受付を撮影成功として扱わない。

IBL は `ibl.bake` の後に `ibl.inspect` の `state` を確認する。描画パスは
`renderpass.configure` で捕捉を有効にし、対象ビューの描画後に `inspect/capture` を呼ぶ。
出力名・ビュー・表示設定が一致しない古い画像を、現在の要求の結果として返さない。

参照検索は保存済みファイルのスナップショットであり、動的参照・未保存・外部所有者・
推移参照を証明しない。`finished/limited/stale` と未読・未対応・解析失敗の件数を確認する。
検索後にプロジェクトが変わった場合、古いプロジェクトの読取りを進めない。

## 実装の接続点

- `IPanel::OnRegisterOperators` は生成後に一度呼び、パネル固有の操作も同じ登録簿へ載せる。
- `IPanel::OnUpdate` は表示状態に依存せず呼ぶ。シーン復帰を反映した後で進める。
- `IPanel::OnRenderFrame` は GPU フレーム内で呼ぶ。Fluid と解析ボリュームは、ここで表示状態に依存せずプレビューや完了処理を進める。
- Project Settings・入力・EditorSettings・Sequence は通常入出力と API の値解析を共用する。
- アセットのファイル操作と Create の既定内容は UI と API の共通関数へ集約する。
- C++ の回帰テストは `Projects/Tests/Editor/Auto`、実操作は通常の Command Bus を呼ぶ Playtest で確認する。
