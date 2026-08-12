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
  `vfx_node_get_field` / `vfx_runtime_state` / `vfx_analyze_texture` / `vfx_analyze_material` / `vfx_survey_assets` /
  `vfx_preview_ensure` / `vfx_preview` / `vfx_preview_sequence` / `vfx_preview_metrics` / `vfx_preview_curve` /
  `vfx_preview_compare` / `vfx_preview_sweep` /
  `shader_inspect` /
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
  `vfx_graph_set` / `vfx_node_duplicate` / `vfx_node_set_metadata` / `vfx_node_set_parent` /
  `vfx_link_update` / `vfx_param_remove` / `vfx_param_unbind` /
  `vfx_variant_remove` / `vfx_group_add` / `vfx_group_update` / `vfx_group_remove` /
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
- `vfx_inspect_graph`: `.vfx`を構文エラーとDAG検証に分けて読み、ノードの有効状態、イベントTrigger、SubGraph、Particle方式、Burst数、budget使用量を返す。不正グラフも構造を失わずAIが修復方針を立てられる。`vfx_node_set_enabled`で配線を保ったまま個別ノードを有効化・無効化できる。加えて各ノードの`transform`(position/rotationDegrees/scale)・`parentNodeId`・`attachBone`・`editorPosition`、Graphの`budgetLimits`、Group/Note一覧を返すため、実行内容だけでなくCanvas上のオーサリング情報も読み書き対称になっている。ただしそれらは`detail="full"`のときだけで、既定の`detail="summary"`はノードの`transform` / `editorPosition` / Group / SignalNodeの中身を返さない — 構造の把握にはそれで足りるのに、テンプレートを一巡見るだけで座標が応答の大半を占めていたため。空間配置やCanvas配置を編集するときだけ`full`で読み直す。
- `vfx_node_get_field`: `vfx_node_set_field`の対になる読み出し。書く手段はあるのに読む手段が無く、`blendMode` / `texturePath` / `colorGradient`に**今何が入っているか**をAPI越しに確かめられなかった（`node_get_components`はScene側のノード用で、VFX Graphの`nodeId`とは別空間なので使えない）。現在値を知らないまま書くと、変更が効いたのかどうかもプレビュー画像からしか判断できず、反復が「変えて見る」の繰り返しになる。返る`value`は`vfx_node_set_field`の`value`と同じ表現なので、読んで一部だけ変えて書き戻せる（Curve/Gradientは`{interp, keys}`、enumには`enumName`が付く）。`schemaPath`指定で1つ、省略で全leaf、`prefix="particle."`で部分木に絞る。
- `vfx_preview_ensure`: VFX Preview Worldを起動し、プレビュー系を呼べる状態かを返す。**Preview Worldを所有するのはEditor本体ではなく独立プロセス`FBZZVFXEditor.exe`**で、Editor側の`vfxPreviewScene`は常に空。そのため`vfx_preview` / `vfx_preview_metrics` / `vfx_preview_curve` / `vfx_runtime_state`は、その独立プロセスへ転送されない限り必ず`NO_PREVIEW_WORLD`になる（転送は`VFXEditorLauncher::ShouldRouteRequest`が判定し、居なければ起動して初期化完了まで待つ）。このツールは起動と存在確認そのものを公開するので、`vfx_guide`が勧める`lint → previewMetrics → previewCurve`の後半2つが「呼べない理由も分からないまま失敗する」状態を無くす。`readyForPreview=false`のときの原因は`rendererReady`に出る。プロセスが未起動だった場合、転送側は最大6秒まで接続を待つ（その間Editorのメインスレッドは止まる）ので、初回だけ数秒かかる。MCP側の既定タイムアウトは10秒で、足りなければ`FBZZ_EDITOR_BUS_TIMEOUT_MS`で伸ばす。起動・接続の失敗はEditor側のログに`VFXEditorLauncher:`付きで残るため、`console_get_logs`で原因を追える。
- **VFX Editor機能数の完全化**: `vfx_graph_set`（名前・budget）、`vfx_node_duplicate` / `vfx_node_set_metadata`（表示名・Canvas座標）、`vfx_link_update`、`vfx_param_remove` / `vfx_param_unbind`、`vfx_variant_remove`、`vfx_group_add` / `update` / `remove`を追加した。従来の追加・値変更・削除だけでなく、VFX Editor UIが保存する主要面をAIからもUndo可能にCRUDできる。公開パラメーター削除はbinding・Variant override・SubGraph forwardを同時に掃除し、参照切れを残さない。
- **VFXの2つの軸**: `link`は「いつ発火するか」だけを決め、位置には一切関与しない。空間の入れ子は`parentNodeId`が担い、`vfx_node_set_parent`で編集する。linkで繋いだだけでは位置は継承されず、親子にしただけでは発火順は変わらない。この混同はAIが最も踏みやすいので、`vfx_guide`の`hierarchy` topicが同じことを規約として返す。親にできるのは実体を持つノードだけで、Entry/Delayを指定すると`PARENT_HAS_NO_TRANSFORM`、子孫を指定すると`PARENT_CYCLE`で拒否する。
- `vfx_analyze_texture`: 素材テクスチャの画素を観測し、特徴量と推奨オーサリング値を返す。これまで素材はAIにとってパス文字列でしかなく、`blendMode`も`alphaSource`も`spriteColumns`もファイル名からの推測だった。観測すれば機械的に決まる項目は多い — アルファチャンネルが実データを持つか（無ければ`alphaSource=Luminance`が必須で、そうしないと粒子が矩形の板になる）、全画素で`RGB <= A`か（事前乗算。Alphaブレンドで使うと縁が黒く縁取られる）、タイル境界の不連続からflipbookのコマ割り、中心の輝度ピーク（発光する芯=Additive向き）、縁のアルファ勾配（硬ければ`softParticles`）。`recommendations[].schemaPath`と`.value`は`vfx_node_set_field`へそのまま渡せ、`.reason`に根拠が付く。**同じ`AnalyzeTexture`をEditorのVFX Inspectorも呼ぶ**ので、人とAIが同じ根拠を見る（別実装にすると必ずドリフトする）。
- `shader_inspect`: シェーダーが公開する変数とテクスチャスロットの目録を返す。`.mat`の`params`もVFX Meshノードの`animatedParam`も「シェーダー変数名」を要求するが、その一覧を知る手段がこれまで無かった。**存在しない名前を書いても保存は通り、実行時に黙って無視される**ため、「値を変えても絵が変わらない」という形でしか現れず綴り違いに気付けない。HLSLを読ませるのは現実的でないうえ、未使用変数はコンパイル時に消えるので宣言を読むだけでは不十分で、実際に効くのはバイトコードのリフレクション結果（`ShaderDescriptor`）だけ。`components`は渡す値の個数で、float3の変数へ1個だけ渡すと残りは0になる。`vfx_lint`が`animatedParam`をこの目録と照合し、`UNKNOWN_SHADER_PARAM`として報告する。`vfx_analyze_material`も`.mat`の`params`を同じ目録と突き合わせ、存在しないキーと要素数不足を`findings`へ出す。
- `shader_get_compile_diagnostics`: DX11/DX12のオンデマンドコンパイルと外部`compile_shaders.ps1`が返したエラー・警告を、VFXEditorのShader Compile Error表示と同じレジストリから返す。path、entryPoint、target、コンパイラ本文を含み、同じstageの再コンパイル開始時に古い結果を除くため、修正後に一覧から消えたことまでAIが確認できる。
- `vfx_analyze_material`: `.mat`とそのalbedoテクスチャを併せて解析する。**ParticleEmitterに`materialPath`を設定すると、実行時に`blendMode`が`.mat`の値で上書きされる**（`materialPath`を描画設定の単一の信頼元にする設計）。つまりEmitter側の`blendMode`は保存されても使われず、「Additiveにしたのに Alphaで描かれる」という形でしか現れない。`findings`は`.mat`自体を直すべき問題（`blend_mode` / `render_path`がparticleでない / albedo未設定）、`recommendations`は`.mat`では表現できずEmitter側にしか無い設定（`alphaSource` / `spriteColumns` / `sortMode`）に分けてある。`blendModeConflictsWithTexture=true`は、albedoテクスチャの中身が要求するブレンドと`.mat`の宣言が食い違っている状態。
- `vfx_survey_assets`: プロジェクトの素材を分類し、エフェクトの層構成に対して何が足りないかを返す。`vfx_guide`のrecipeは「煙/外炎/芯/火の粉/陽炎」のような層を要求するが、その層を作れる素材が手元にあるかは別問題で、AIは素材を1枚ずつ解析して初めて種類が判る。無い素材を前提にしたグラフを組むと後から代替を探し直すことになるため、`.vfx`をゼロから組む前に棚卸しを返す。`missingRoles`（core / body / sparks / animated）には代替案がhintに付く。解析は1枚あたり数十msかかるため既定120枚で打ち切り、`truncated`で判る。分類結果はファイルの`size`+`mtime`をキーにプロセス内キャッシュへ載るので、同じプロジェクトを繰り返し調べても再解析しない（内訳は`freshlyAnalyzed`/`fromCache`。外部ツールで素材を差し替えたのに分類が変わらないときだけ`refresh=true`）。既定の`detail="summary"`は1枚あたりpathだけを返し、個別の中身は`vfx_analyze_texture`で見る。**全素材の棚卸しが要るのは新しくグラフを組むときだけ**で、既存`.vfx`のlint確認では呼ばず`directory`で絞る。
- `vfx_runtime_state`: 直前の`vfx_preview`が構築した実行状態と実測コストをその時刻のまま返す。ノードが画に出ない原因は「起動していない」「イベント待ちのまま起動しない」「起動しているが見えない」の3通りあり、画像からは区別できない。`vfx_lint`は静的解析なので1つ目しか見えず、2つ目はスケジュール表に位置を持たないため時刻からも推測できない。`waitingForEvent`を返すことで前2つを即断でき、残った3つ目だけが画像で見るべき問題になる。参照するのはAI capture専用World。`simulation.requested`と`simulation.effective`を分けて返すのは、`simulationMode = Gpu`にしても11個の条件のどれか1つで**黙ってCPUへ縮退する**ため — 縮退に気づかないまま粒子数だけ増やすと性能は一切使われない。`cost.particlePassGpuMs`はParticleパスの実測GPU時間、`cost.overdraw`は重なり枚数の集計（`vfx_preview`を`view="overdraw"`で実行したときだけ計測。読み戻しはGPU同期でフレームを止めるため常時は測らない）。
- `vfx_preview_metrics`: プレビュー画を「絵」ではなく「数値」として読む。`vfx_preview`はPNGを返すだけで判断が全て視覚に委ねられていたため、同じ画から毎回違う結論が出て「少し暗い」の“少し”に基準が無く、直す量が決められず反復が振動していた。読み戻しはPNGとは別経路でHDR線形値のまま行う（`CaptureRenderTargetToLinearRGBA`）— 8bitへクランプすると白飛びと「単に明るい」がエンコードの時点で区別できなくなる。閾値の解釈をクライアント側にぶれさせないため、**判定そのものをエンジン側に置き**、`BLOWN_OUT` / `OVEREXPOSED` / `SCREEN_FLOODED` / `TOO_DIM` / `EMPTY_FRAME` / `STATIC_FRAME` / `OFF_CENTER`を`issues`として名指しで返す。`issues`が空なのは「機械的に判る破綻が無い」であって「良い絵」ではない。
- `vfx_preview_curve` / `vfx_preview_compare`: エフェクトの質は静止画ではなく**時間の形**（立ち上がりの速さ・ピークの位置・消え際の粘り）で決まるが、t=0/peak/endの3枚を見ても「立ち上がりが鈍い」は判定できない。`vfx_preview_curve`は全区間の指標を時系列で返し、`peakNormalized`（ピーク位置を全長で正規化）と`tailRatio`（消え際の残り）を添える。`vfx_preview_compare`は2つの`.vfx`を同じ時刻列で測って差を符号付きで返すので、「coverageが1.8倍・輝度は同じ = コストだけ増えた」が言える。画像2枚からは「良くなった」のか「変わっただけ」なのかを言えない。
- `vfx_knowledge_catalog` / `vfx_candidate_fork` / `vfx_candidate_evaluate` / `vfx_candidate_accept` / `vfx_knowledge_promote`: 生成・評価・採択・知識化を一つの閉ループにする。元アセットを直接上書きせず成功Templateから候補を分岐し、同じobjectiveでlint・全区間メトリクス・意味評価を通した候補だけ本番へ採択する。機械条件を通っても`semanticAssessment`が無ければ`needs_semantic_review`のままで、数値だけによるAAA認定はしない。採択結果は`Assets/VFX/Templates`の再実行可能な`.vfx`へ昇格し、次回の生成元になる。
- `vfx_preview`の`camera` / `vfx_preview_sweep`: 視点を注視点まわりの球面座標（`preset` / `distance` / `yaw` / `pitch`）で指定する。自由なカメラ行列を組ませると同じ「斜め上から」を再現できず評価が揺れるため、意図的に自由度を絞ってある。ビルボードは横から見ると平面なので、シルエットの破綻は`yaw:90`でしか判らない。`vfx_preview_sweep`は距離を振って評価するので、`lodNearDistance`/`lodFarDistance`の切り替わり確認と「ゲーム内距離で読めるか」が1コマンドで済む。
- `vfx_lint`の`fix` / `autoFixable`: 各issueに「どのツールをどう呼べば直るか」を機械可読で添える。以前は何が壊れているかだけを返し直し方はAIの推測に任せていたため、同じ警告に対して呼ぶコマンドが毎回変わり、直したつもりで別の規約を踏む往復が発生していた。`caution`がある項目は、その修正で失われるものを確認してから実行する。
- `vfx_repair`: `autoFixable=true`の code を機械的に直す。`UNREACHABLE_NODE` / `MISSING_ASSET` / `SHEARED_SPRITE` / `LIGHTING_SATURATED` / `ALPHA_NO_SORT` / `MESH_NO_FADE` / `PARENT_HAS_NO_TRANSFORM`。直し方が一意に決まるものだけを扱い、「何を出すか」のような設計判断には触れない。`GPU_FALLBACK`は`autoFixable=false` — 「GPUで大量」と「正しいソート / per-particle Trail / SubEmitter」は両立せず、どちらを捨てるかは表現の要求次第で機械的には決められないため。
- `vfx_optimize_budget`の`strategy`: パーティクルの実コストは粒子数ではなく塗った画素数（fill rate）で決まるため、原因がfill rateのときに粒子数だけ減らすと効きが悪く見た目だけが痩せる。`particles`（従来の比例削減）/ `fillRate`（粒を大きくして枚数を減らす + 寄与の大きい層を遠距離で間引く）/ `both`から選ぶ。どれを使うかは`vfx_runtime_state`の`cost.overdraw.meanLayers`と`cost.particlePassGpuMs`で判断する。`fillRate`の粒サイズ補正が`k^(1/3)`倍なのは、`√k`にすると総塗り面積が変わらずfill rate対策にならないため。
- `scene_get_tree` / `scene_snapshot`は、システムが実行時に生成したGameObject(`runtimeGenerated`)を既定で除外する。VFX Graphは1エフェクトにつきノード数ぶんのGameObjectを作るため、混ぜるとcontextを食い潰したうえ、編集しても保存されない対象への無駄な編集を誘発する。除外件数は`hiddenGeneratedChildren` / `excludedGenerated`に出るので「無い」とは読ませない。実行中の実体を調べたいときだけ`includeGenerated=true`。
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
