# AI 検証ループ

## 目的

AI がエディターを操作する口 (Command Bus / MCP / Operator) は揃った。欠けていたのは、AI が**自分の変更の合否を自分で出す**口。
「変更 → ビルド → プレイ → 絵の確認」を人の手を介さずに一周できるようにする。

| # | 欠けていたもの | 足したもの |
|---|---|---|
| 1 | エンジン C++ をコンパイルできない | `Tools/AgentBuild.ps1` (1 ファイル単位のコンパイル・ターゲットビルド・ctest を file:line で要約) |
| 2 | プレイの合否をコードで判定できない | Playtest シナリオ (`*.playtest.json`) と `PlaytestRunner`。固定 dt のロックステップ・待機・表明・入力の記録/再生 |
| 3 | 絵の回帰を判定できない | `ImageCompare` と基準画像。シナリオの `compareImage` 手順と `visual.compare` |
| 4 | エディターを起動しないと何もできない | `FBZZEditor --batch <scenario>`。窓を出さずにシナリオを回し、終了コードとレポートを返す |
| 5 | 規約が文章のまま | `Projects/DevTools/AgentLint/lint.mjs` を Claude Code の PostToolUse フックに掛ける。複数ファイル手順は `.claude/skills/` |
| 6 | バスが 8000 行の if 連鎖 | 領域ごとのファイルへ分け、型名 → ハンドラーの表で引く。表と MCP 契約の食い違いはテストで落とす |

## 1. AgentBuild

- **ターミナルからのビルド禁止は «VS 開発者環境を知っているのが `VcBuild.ps1` だけ» が理由。** `AgentBuild.ps1` は同じ `Import-VisualStudioEnvironment` を通るので規約と両立する。AI が叩いてよいビルドの入口はこれだけ。
- `check <files...>`: 所有する `.vcxproj` を探して `ClCompile` + `SelectedFiles` でコンパイルだけ行う (リンクしない = 起動中のエディターが DLL を掴んでいても通る)。ヘッダーを渡すと、それを include する `.cpp` を最大 3 本選んで代わりにコンパイルする。どの `.vcxproj` にも無いファイルは GLOB が古いので 1 度だけ再 configure する。
- `build <target>` / `test [-Filter]`: `cmake --build` / `ctest`。LNK1168 はエディター起動中として分類して返す。
- 出力は `ERROR path:line:col CODE message` の 1 行形式に畳み、重複を落とす。全文は `RESULT` 行の log= (最新は `build/agent/last-<verb>.txt` が指す)。終了コード 0 = 成功 / 1 = コードのエラー / 2 = 環境の失敗。
- 構成ツリーは `development → debug → release` の順で CMakeCache のあるものを使う。
- **ビルドは同時に 1 本だけ。** SDK 公開も `build/<Config>` から行うので、人のタスクと AI の検証は同じツリーで重なりうる。重なった MSBuild は中間ファイルを奪い合い、C1041 / C1083 / LNK1104 / MSB3491 という «コードのエラー» の形で落ちる。AI がそれを直そうとして正しいコードを壊すのを防ぐため、AgentBuild は始める前に `cl.exe` / `link.exe` / `MSBuild.exe` (ノード再利用の待機を除く) / `cmake.exe` / `ctest.exe` を探し、あれば `RESULT busy` (終了コード 2) で断る。途中で重なった形跡は `HINT CONTENDED` で返す。上書きの引数は持たせない (逃げ道があると常に使われる)。対処は verify-cpp スキルの «ビルドが重なったとき»。

## 2. Playtest

### ロックステップ

`Time::SetLockstepDelta(dt)` が 0 より大きい間、`Time::Tick` は実時間を測らず固定 dt を返し、FPS キャップの待機もしない。
シナリオは既定で `1/60` を掛ける。«N フレーム後» と «N/60 秒後» が一致し、描画が遅い機械 (WARP・CI) でもゲーム内の進みが同じになる。
終了・中断時は必ず 0 へ戻す (戻し忘れるとエディターの時間が実時間から外れる)。

### シナリオ形式

```json
{
  "name": "Stage01 起動",
  "scene": "Assets/Scenes/Stage_01.scene",
  "lockstep": 0.016666667,
  "steps": [
    { "do": "play" },
    { "do": "frames", "count": 30 },
    { "do": "bus", "request": { "t": "input.inject", "kind": "key", "key": "W", "pressed": true } },
    { "do": "waitUntil", "query": { "t": "scene.find", "name": "Player" }, "path": "nodes.0.name", "op": "exists", "timeoutFrames": 600 },
    { "do": "assert", "query": { "t": "console.logs", "minLevel": "error" }, "path": "entries", "op": "length==", "value": 0 },
    { "do": "compareImage", "view": "game", "baseline": "Stage01_start", "maxMeanDiff": 0.02, "maxBadPixelRatio": 0.01 },
    { "do": "stop" }
  ]
}
```

- **手順の語彙は Command Bus そのもの。** `bus` / `waitUntil` / `assert` はバスの要求をプロセス内で `EditorBusDispatcher::Handle` へ流す。シナリオ専用の問い合わせ API を作らないので、MCP で読めるものは全部シナリオで表明できる。
- `path` はドット区切り (`nodes.0.name`)。`op` は `exists` / `missing` / `==` / `!=` / `<` / `<=` / `>` / `>=` / `contains` / `length==` / `length>=`。
- `waitUntil` は毎フレーム評価し、`timeoutFrames` を越えたら失敗。`frames` は単に進める。
- `replay` は記録した入力列 (`*.inputrec.json`) をフレーム番号どおりに注入する。
- 手順が 1 つ失敗した時点で打ち切り、Play を止めてレポートを書く (後続の手順は前提が崩れているため)。

### 入力の記録と再生

- 記録はエディターの Play 中に `input.record` (start/stop) で行う。毎フレーム、キー 256 本・マウスボタン・マウス移動量・仮想軸/ボタンの**変化だけ**を `{ frame, inject }` で残す。
- 再生は同じ `input.inject` を同じフレーム番号へ当てる。ロックステップ下で記録すれば再生も同じ dt になる。
- **決定性は保証しない。** 乱数の種・非同期ロード・GPU パーティクルは揺れる。再生は «同じ操作を当てる» までで、結果の一致はシナリオの表明で見る。

### 実行の口

| 口 | 用途 |
|---|---|
| バス `playtest.run` / `playtest.status` / `playtest.cancel` / `playtest.list` (MCP `scenario_run` / `scenario_status` / `scenario_cancel` / `scenario_list`) | エディター起動中に AI が回す。非同期で、状態を問い合わせて待つ |
| `FBZZEditor --project <p> --batch <scenario> [--report <json>] [--update-baselines] [--skip-images] [--hidden] [--warp]` | エディターを人が使わずに回す。終了コード 0 = 全合格 / 1 = 不合格 / 2 = 起動・読込の失敗 |

- レポートは `<Project>/Library/Playtests/<name>/report.json`。実画像・差分画像も同じ場所 (Library は追跡しない生成物の置き場)
- 手順の実行は `IModule::OnInputPolled` (OS 入力の読み取り後、`InputActionMap::Update` の前)。注入した入力が同じフレームのアクション層に届く
- MCP には以前から `playtest_run` (MCP 側で実時間待機する簡易版) がある。こちらはフレーム単位で進みファイルに残せる別物なので `scenario_*` と名付けた
- バッチは再起動系 (`RelaunchOutsideKillOnCloseJob` / エンジン鮮度チェック) とモーダルを飛ばす。再起動すると呼び出し側が受け取る終了コードが «0 で抜けた親» になるため

## 3. 絵の回帰

- 基準画像は `<Project>/Tests/Golden/<name>.png`。`Assets/` の外に置く (`.meta` が付かず、アセットとして読まれない)。
- 比較は RGB の画素ごとの差 (0..1)。`meanDiff` (全画素平均) と `badPixelRatio` (差が `pixelThreshold` 既定 0.1 を越えた画素の割合) の両方で判定する。平均だけだと小さな欠け (HUD が 1 個消えた) を見逃し、割合だけだとトーンの全体ずれを見逃す。
- 基準画像が無ければ実画像だけ出して **失敗** (`missing-baseline`)。`--update-baselines` のときだけ実画像で基準を書き、`updated` として手順は通す。作った絵は人か AI が目で確かめてから commit する
- 単発の下見は `visual.compare` (MCP `visual_compare`)。差分画像を応答に載せる
- 寸法が違えば比較せず失敗。
- 差分画像は差を赤で強調した PNG。AI はこれを読んで «どこが変わったか» を言える。
- TAA のジッター・時間で動くシェーダーがあるため、固定カメラ・ロックステップ・一定フレーム後で撮る。

## 4. バッチ実行

- `--batch` は通常のエディター起動と同じ `EditorApp` を使う (別の «ヘッドレス実装» を作ると、エディターで通る/通らないが食い違う)。違うのは: 窓を出さない (`--hidden`)、AI バスを開かない、モーダル確認を出さない、シナリオ完了で `Application::Quit`。
- Game View は非表示でも描く (`aiViewportRenderUntilFrame` をシナリオの間延ばし続ける)。
- `--warp` で DX12 を WARP (ソフトウェア) アダプターに落とす。GPU の無い CI 用。WARP の絵は GPU と一致しないので、画像比較は `--skip-images` で飛ばすか WARP 専用の基準画像を持つ。

## 5. 規約の機械化

- `node Projects/DevTools/AgentLint/lint.mjs <files...>` / `--changed` (git の差分)。PostToolUse フックでは編集したファイル 1 本を見る。コメント規約の詳細は `Docs/conventions/comments.md`
- 共有するのは `.claude/settings.json` (フックと AgentBuild / lint の許可) と `.claude/skills/` (verify-cpp / playtest / add-bus-command / add-script-proxy / add-script)。`settings.local.json` は個人設定なので追跡しない
- **error** (フックが止める): `new` / `delete` 式、`throw`、`dynamic_cast`、`std::ranges`、コルーチン、`import` モジュール、ヘッダーの `<Windows.h>`、エンジンヘッダーの `using namespace`、`.generated.hpp`、禁止ライブラリの include、生成物 (`ScriptList.inl` / `DataAssetList.inl`) の手編集、4 行ヘッダーの欠落、シェーダー複製の不一致、ツール置き場 (`Tools/` / `Projects/DevTools/` / `<Project>/Tools/`) の索引 `README.md` に無いファイル (`tool-unlisted`。拡張子を問わない)、ASCII 以外を含むのに BOM の無い `.ps1` (`ps1-bom`。Windows PowerShell 5.1 が CP932 で読んで構文エラーになる)。
- **warn** (文脈へ返すだけ): `//` の自由記述コメント (旧コードに大量にあるため止めない)、`reinterpret_cast` (定数バッファ転送のみ可)、テスト `.cpp` の CMake 登録漏れ。
- 判定はコメントと文字列リテラルを空白に潰してから行う (`= delete` と «new» という単語を誤検出しない)。

## 6. バスの分割

- `Projects/Editor/src/Ai/Bus/` に領域ごとのファイル (`SceneHandlers.cpp`, `AssetHandlers.cpp`, …) を置く。各ファイルは `RegisterXxxHandlers(BusHandlerTable&)` を公開する。
- **自己登録の静的初期化子は使わない。** FBZZEditor は STATIC ライブラリで、参照されない .obj の初期化子はリンカーに捨てられる。`EditorBusDispatcher` のコンストラクタが登録関数を明示的に順に呼ぶ。
- 表は `{ type, kind (Query/Command), handler }`。同じ型の二重登録は assert。`editor.bus.list` で型名の一覧を返す。
- `EditorAuto` のテストが `Projects/EditorMcp/src/editorContracts.ts` の `t: '...'` を全部読み、表に無い型があれば落ちる (MCP だけに生えて C++ に無い / 綴り違いの検出)。

## 検証

- AgentBuild: `check` で既知のエラーを 1 つ入れた TU が `ERROR` 行を返し、直すと 0 で終わること。
- Playtest: `ScenarioPath` / `JsonPath` / 比較演算 / 画像比較 / 入力記録の差分抽出を EditorAuto で確認する。ロックステップは CoreAuto。
- バス分割: 既存の `EditorBusDispatcherTests` がそのまま通ること + 契約の突き合わせテスト。
- 実機: GreenWare で `--batch` を回し、終了コードとレポート・差分画像を確認する。
