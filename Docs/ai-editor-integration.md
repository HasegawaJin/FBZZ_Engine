# AI (Claude) 連携 — Editor Command Bus / MCP

現行の C++/ImGui エディターに **Claude** を接続し、AI がシーンを「知覚」して「操作」できるようにする仕組み。

## アーキテクチャ

```
Claude (Code / Desktop)
   │  stdio (MCP JSON-RPC)
   ▼
Projects/EditorMcp/dist/stdio.js        … MCP サーバ (TypeScript / Node)
   │  Windows Named Pipe  \\.\pipe\FBZZEditorCommandBus  (NDJSON)
   ▼
[C++ Editor]  NamedPipeServer(ワーカースレッド)
   │  スレッドセーフキュー
   ▼  EditorApp::OnUpdate (メインスレッド)
EditorBusDispatcher → Scene / UndoStack / ComponentRegistry / JsonReflector / IRenderer::CaptureRenderTargetToPng
   ▲
   └─ 応答 (NDJSON) を Claude へ返す
```

- **プロトコル**: `fbzz.editor.v1`。要求 `{protocol,id,kind,payload,dryRun,source}` / 応答 `{protocol,id,ok,result?,error?}`。
- **NodeId**: `GameObject.instanceId` (UUID v4)。リネーム・再ロードに耐える安定 ID。
- **スレッド境界**: IO はワーカースレッド、シーン変更と RT 読み戻しは必ずメインスレッド (`OnUpdate` の `DrainRequests`)。
- 実装: C++ 側 `Projects/Editor/{include,src}/Ai/`、MCP 側 `Projects/EditorMcp/`。

## 権限モード (`FBZZ_MCP_PERMISSION`)

| モード | 公開ツール | 用途 |
|--------|-----------|------|
| `read` (既定) | Query のみ (`scene_get_tree` / `viewport_capture` 等)。書込ツールは**発見不能** | まず疎通確認 |
| `dry-run` | Command も公開するが `dryRun=true` を強制し、シーンを変更せず試算のみ | 影響確認 |
| `write` | 実変更を許可 (すべて Undo 可能) | 実作業 |

## セットアップ

### 最短ルート: Editor 内ワンストップ (AI メニュー)

Editor のメニューバー **AI** に環境診断とセットアップが集約されている。CLI や config の手編集は不要:

1. **AI → Setup** の診断で `node.exe` / `MCP サーバ (dist/stdio.js)` が OK であることを確認
   （dist 未ビルドなら下記「MCP サーバをビルド」を1回だけ実行）
2. **AI →「Claude Desktop へ登録 (write)」** — `claude_desktop_config.json` へ 1 クリック登録
   （既存の他 MCP 登録は保持。read で登録すれば読み取り専用で試せる）
3. **AI →「Enable Command Bus」** を ON
4. **AI →「Claude Desktop を起動」** — 登録後に再起動が必要な場合もここから

Claude Code (CLI) 派は **「Claude Code 登録コマンドをコピー」** でコマンドを取得しターミナルへ貼るだけ。

以下は手動でセットアップする場合の手順。

### 1. MCP サーバをビルド

```sh
cd Projects/EditorMcp
npm install      # 初回のみ (node_modules は .gitignore 済み)
npm run build    # dist/ を再生成
npm test         # 任意: 21 テスト
```

`dist/` はコミット済みなので、`npm install` 済み環境なら `npm run build` を省いてそのまま登録してもよい。

**VSCode からビルドする場合**（初回の `npm install` 後）:
- タスク: `Terminal → Run Task → EditorMcp: Build`（テストは `EditorMcp: Test`）
- Run & Debug: **「EditorMcp Server (Build & Run)」** を選ぶと `EditorMcp: Build` を実行してから
  `dist/stdio.js` を起動する（起動確認・stderr ログ確認用。通常の常用は Claude 側が起動する）。

### 2. Editor を起動し、AI メニューから Command Bus を有効化

VSCode / Visual Studio から Editor をビルドして起動する（**ターミナルからビルドしない**規約）。
起動しただけでは待受しない。メニューバーの **AI →「Enable Command Bus」** で待受を開始する
（ログに `AI Command Bus listening ...`、メニューに「状態: 待受中」）。この設定は EditorSettings に
永続化されるため、次回起動時は自動で待受が再開する。
MCP は要求毎に再接続するため、Editor と Claude はどちらを先に起動してもよい。

### 3-A. Claude Code (CLI) へ登録

```sh
# <repo> は本リポジトリの絶対パスに置き換える
claude mcp add fbzz-editor \
  -e FBZZ_MCP_PERMISSION=read \
  -- node <repo>/Projects/EditorMcp/dist/stdio.js
```

実作業時は `FBZZ_MCP_PERMISSION=write` にして再登録する。

### 3-B. Claude Desktop へ登録

`claude_desktop_config.json`（Windows: `%APPDATA%\Claude\claude_desktop_config.json`）に追記:

```json
{
  "mcpServers": {
    "fbzz-editor": {
      "command": "node",
      "args": ["<repo>/Projects/EditorMcp/dist/stdio.js"],
      "env": { "FBZZ_MCP_PERMISSION": "read" }
    }
  }
}
```

Claude Desktop を再起動すると `fbzz-editor` ツール群が現れる。

## MCP ツール一覧

- **Query (read+)**: `editor_catalog` / `editor_catalog_search` / `editor_get_state` / `editor_get_undo_history` / `console_get_logs` /
  `scene_find` / `scene_get_tree` / `scene_snapshot` / `scene_diff` / `scene_validate` / `scene_get_selection` /
  `node_get_components` / `asset_list` / `asset_inspect` / `asset_find_unused` / `asset_thumbnail` / `vfx_inspect_graph` /
  `material_inspect` / `animation_get_state` / `animation_get_graph` / `animation_get_blend_tree` /
  `animation_get_pose` / `profiler_get_snapshot` / `physics_raycast` /
  `physics_overlap_sphere` / `physics_get_events` / `editor_wait` / `viewport_capture` /
  `viewport_capture_semantic` / `editor_perceive`
- **Command (dry-run/write)**: `node_create` / `node_duplicate` / `node_delete` / `node_reparent` / `node_rename` /
  `node_set_active` / `node_set_tag` / `node_set_layer` /
  `selection_set` / `transform_set` / `component_add` / `component_set` / `component_remove` /
  `asset_import` / `prefab_instantiate` / `material_assign` / `material_set_parameter` / `material_set_shader` /
  `animation_control` / `animation_set_parameter` / `animation_add_transition` / `animation_set_condition` /
  `animation_remove_transition` / `animation_add_state` / `animation_set_state` / `animation_remove_state` /
  `animation_add_motion` / `animation_set_motion` / `animation_remove_motion` /
  `animation_add_parameter` / `animation_remove_parameter` /
  `input_inject` / `play_control` / `playtest_run` / `viewport_camera_set` /
  `editor_undo` / `editor_redo` / `run_transaction` / `run_transaction_and_observe`

自律ループは `editor_perceive → (dry-run/write) Command → editor_perceive` を反復し、
Scene 状態と viewport の見た目の両方で完了条件を確認する運用を想定。

### 発見性と構造操作

- `editor_catalog`: `ComponentRegistry`と各`Reflect()`を正本に、公開コンポーネントの正確な型名、表示名、分類、追加可否、編集可能フィールドを返す。フィールドには型・既定値に加え、宣言されているenumラベル、range、group、tooltipを含む。
- `scene_find`: `name`部分一致、`tag`、`active`、`comp`（互換入力）、`components[]`、`properties[]`をAND条件で検索する。
  プロパティ演算子は `equals` / `notEquals` / `contains` / `greater` / `less`。結果は安定NodeId、名前、タグ、active、階層パス、親NodeIdを含む。
- `node_duplicate`: 指定ノードの子階層と全コンポーネントを複製する。親・複製名は任意指定でき、操作全体を1回のUndoで取り消せる。再帰コピーの循環を防ぐため、元ノード自身またはその子孫は複製先に指定できない。
- `node_set_active` / `node_set_tag` / `node_set_layer`: GameObjectのactiveSelf(有効/無効)・tag・layer(0〜31)をUndo可能に設定する。照会側 (`scene_get_tree` / `scene_find` / `scene_snapshot` / `node_get_components`) が同じ active/tag/layer を返すので、読み取り→設定が対称。`node_set_active`で無効化すると子孫ごと非表示・停止になる。
- `play_control`: `start` / `stop` / `pause` / `resume` / `step`。`stop`直後は安全な次フレーム復元待ちになるため、`editor_get_state`の`restorePending=false`まで確認する。
- `editor_get_undo_history`: Undo/Redoスタックを新しい順で返す。canUndo/canRedo、cursor、次にundo/redoされる操作の説明、履歴エントリ(index・ラベル・applied)を含む。自分の編集が期待どおりのラベル(例:`AI: Add Animator State`)で残ったか検証したり、何回`editor_undo`で戻れるか判断するのに使う。read権限で利用可。
- `console_get_logs`: 重大度・本文部分一致・件数で絞り込み、新しい順に返す。応答の単調増加`cursor`を次回`afterSequence`へ渡すと、今回の操作後に発生したログだけを取得できる。リングバッファから取りこぼした場合は`dropped=true`。
- `prefab_instantiate`: projectRoot配下の`.prefab`だけをインスタンス化する。生成ルートのNodeIdを返し、操作はUndo対応。
- `viewport_camera_set`: Scene Viewカメラの座標移動と、座標またはNodeIdへの注視を行う。直後に`viewport_capture`で視覚確認する。
- `physics_raycast` / `physics_overlap_sphere`: Editor Playと同じPhysics Worldを数値照会し、Colliderに対応するNodeIdも返す。
- `viewport_capture` / `viewport_capture_semantic`: `view=scene|game`で取得元を選ぶ。semantic版はPNGに加えて、全GameObjectのNodeId、親、world座標、投影pixel、depth、画面内判定を同一応答で返す。Game Viewは実描画と同じカメラ解決規則とRTアスペクト比を使う。
- `scene_snapshot` / `scene_diff`: MCPプロセス内に最大16件の正規化Scene状態を保持し、NodeId単位の追加・削除・変更を比較する。
- `scene_validate`: 重複/空NodeId、重複名、NaN/Infinity/ゼロscale、Prefab/Material/Animator Controller参照切れを一括診断する。
- `asset_inspect` / `asset_find_unused`: projectRoot外を拒否し、テキスト形式アセットの参照先・参照元と未参照候補を調べる。`asset_thumbnail`はPNG/JPEGを画像応答にする。
- `vfx_inspect_graph`: `.vfx`を構文エラーとDAG検証に分けて読み、ノード、イベントTrigger、SubGraph、Particle方式、Burst数、budget使用量を返す。不正グラフも構造を失わずAIが修復方針を立てられる。
- `material_assign` / `material_set_parameter` / `material_set_shader`: MaterialComponentの割当、インスタンス別float/vector上書き、`.mat`のshaderPath変更をUndo対応で行う。
- `animation_control`: clip名/indexまたはAnimator stateを選んで再生・停止し、秒/フレームへシークする。`animation_get_state`で評価時刻、`animation_get_pose`でSkeletonノード名・親・bone index・4x4 global matrixを確認できる。
- `animation_get_graph`: Animatorステートマシンの構造を返す。states（mode・clip・BlendTree駆動パラメーター・遷移リスト）、遷移条件（parameter/op/threshold）、anyState遷移、parameters（型とライブ値）、defaultState/currentState/blendを含み、「どのパラメーターがどの遷移を発火させるか」をAIが把握できる。
- `animation_set_parameter`: Animatorパラメーターを名前で設定して遷移やBlendTreeを駆動する。型は宣言型に従う（float/int=数値、bool=真偽、trigger=真偽で発火/リセット・省略時発火）。`animation_get_graph`で名前と型を確認し、設定→`animation_get_state`/`viewport_capture`で遷移結果を検証する自律ループに使う。`input_inject`より直接的にステートマシンを操作できる。
- `animation_get_blend_tree`: 指定ステートのBlendTreeを掘り下げ、駆動パラメーターとライブ値、各Motionのsource/clip/threshold(1D)またはposX/posY(2D)/speed/IK、直近フレームのランタイムWeightを返す。Clipステートやステート不在はエラー。
- `animation_add_transition`: ステート間（またはAny State→ステート）にUndo可能な遷移を追加する。`from`省略でAny State遷移。遷移先が既にある場合はエラー。Undoは`states`+`anyStateTransitions`の丸ごと復元で、消えたステートを指したまま再生が続かないようランタイム遷移状態も消す。
- `animation_set_condition`: 指定遷移の発火条件を`add`/`update`/`remove`/`clear`でUndo可能に編集する。add/updateは既存`parameter`と`op`が必須（true/false以外は`threshold`も）、update/removeは`conditionIndex`が必須。`transitionIndex`は`animation_get_graph`の`states[].transitions`または`anyStateTransitions`の並び順。編集→`animation_get_graph`で構造を再確認する運用。
- `animation_add_state` / `animation_set_state`: ステートをUndo可能に追加・更新する。addはname重複でエラー、最初のステートや`setAsDefault`でデフォルト化。setは`state`で対象を選び、`name`指定でリネーム（全遷移参照・defaultState・ランタイム名を追従）、mode/clip/speed/loop/ikWeight/BlendTree駆動パラメーター/blend2DType/setAsDefaultを部分更新する。
- `animation_add_motion` / `animation_set_motion`: BlendTreeステートのMotionをUndo可能に追加・更新する。1Dは`threshold`、2Dは`posX`/`posY`で配置し、source/clip/speed/ikWeightを設定する。対象がClipステートやステート不在はエラー。setは`motionIndex`（`animation_get_blend_tree`のmotions並び順）で対象を選ぶ。
- `animation_remove_state` / `animation_remove_transition` / `animation_remove_motion`: それぞれUndo可能に削除する。ステート削除は他ステート/Any Stateからの遷移参照も掃除し、defaultStateだった場合は先頭ステートへ付け替える。遷移削除は`from`(省略でAny State)と`transitionIndex`、Motion削除は`state`と`motionIndex`で対象を選ぶ。
- `animation_add_parameter` / `animation_remove_parameter`: パラメーター自体をUndo可能にCRUDする。addは`type`(float/int/bool/trigger、省略でfloat)と初期`value`を指定し名前重複でエラー。removeは、そのパラメーターを参照する遷移条件を残す（Unity/本エディター同様、無効参照は発火しないだけで壊れず、Undoで丸ごと戻せるためcascade削除しない）。参照状況は`animation_get_graph`で確認する。
- Animator構造編集（transition/condition/state/motion/parameter）はすべて`MakeAnimatorEditCommand`経由で、`states`+`anyStateTransitions`+`parameters`の丸ごとスナップショットでUndo復元する。復元後はランタイム遷移状態を消し、消えたステートを指したまま再生が続くのを防ぐ。`run_transaction`で複数編集を1 Undo単位にまとめられる。
- `input_inject`: Play/Pause中にキー、仮想軸、ゲームパッドボタン/軸、マウス入力を通常Input状態へ合成する。ゲームパッドボタンはScriptInputProxyの`GetButton/Down/Up`、軸は`GetAxis`で読む。Pause→入力→stepで決定的な自動プレイテストを組める。
- `profiler_get_snapshot`: 直近フレームのFPS、CPU sample、draw call、頂点/三角形、カリング数、MemoryTracker統計を返す。
- `physics_get_events` / `editor_wait`: 直近Collision/Triggerイベントを取得し、Play状態・ロード完了・ログ・衝突条件まで非ブロッキングポーリングする。
- `playtest_run`: 呼び出し時点のログカーソルを記録し、Play開始/再開、相対時刻付き入力列、settle待機、今回のログ・FPS/frameMs・物理イベント・意味付きviewportを一括収集する。既定ではGame Viewを検証し、`view=scene`にも切り替えられる。error/warning、性能閾値、必須NodeId可視性、期待ログを判定し、途中失敗でも注入入力をclearして呼び出し前のPlay状態へ復元する。dry-runでは待機せず全Commandの入力検証だけを行う。
- `run_transaction_and_observe`: 実行前後のScene diffに加え、lint、Editor状態、warning以上のログ、意味付きviewportを返して自己検証する。

## 環境変数

| 変数 | 既定 | 説明 |
|------|------|------|
| `FBZZ_MCP_PERMISSION` | `read` | `read` / `dry-run` / `write` |
| `FBZZ_EDITOR_PIPE` | `\\.\pipe\FBZZEditorCommandBus` | 接続先パイプ（`\\.\pipe\FBZZEditor*` のみ許可） |
| `FBZZ_EDITOR_BUS_TIMEOUT_MS` | `10000` | 応答タイムアウト (1000〜60000 にクランプ) |

## 制約・注意 (現状)

- **セキュリティ**: パイプは同一 Windows セッション限定。エンドポイント名は正規表現で固定。
- `component_set` は反射 (`FBZZ_FIELD` 等) されたスカラー・ベクトル・文字列フィールドが対象。
  EntityID/参照型フィールドはシーン解決を要するため対象外（構造変更は `node_reparent` 等の専用 Command で行う）。
- `node_delete` の Undo は単一ノードのスナップショット復元。子階層を持つノードの完全復元は未対応。
- `viewport_capture` は Scene/Game View RT (HDR) を R8G8B8A8 へ変換した PNG。トーンマップは行わない（AI プレビュー用途）。
- `viewport_capture_semantic` の位置はGameObject原点の投影であり、メッシュ輪郭やオクルージョン可視性を表すセグメンテーション画像ではない。
- `asset_find_unused` は2MB以下の既知テキスト形式内に現れる `Assets/` パスを走査するヒューリスティック。バイナリ内参照や動的ロード名は検出対象外。
- DX12 バックエンドのキャプチャは DirectXTex の D3D12 `CaptureTexture` を使う。リンク時に DirectXTex が
  DX12 対応でビルドされている必要がある。
