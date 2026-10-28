# Spatial debug visualization

Scene View の **Overlays → Spatial Visualization**、または既存 **Debug** メニューから操作する。起動時は無効。Render と Physics は独立して有効にでき、パネル内の対象選択は他方を無効にしない。

| 対象 | 階層 | 境界 | 候補 |
|---|---|---|---|
| Render | 実際の影候補抽出用 STATIC / DYNAMIC BVH | 階層ノードのワールド境界 | 影ライト領域の union 照会で観測したノード状態 |
| Physics | 実際の衝突 broadphase の二分BVH | broadphase proxy の境界 | NarrowPhase 前に受理された候補ペアを結ぶ線 |

Render はカメラの可視判定でも個別ライト照会でもない。候補葉には後続の厳密な caster 条件を通過していないものも含む。Physics の候補ペアは接触確定ではない。World の空間クエリ木、三角形メッシュ内のBVH、単なる collider gizmo と区別する。物理の内部ノードに STATIC / DYNAMIC 分割を捏造しない。proxy の色は実際の static 状態から決まる。

## 操作と取得できない状態

- 対象の表示、階層、proxy 境界、候補ペアを個別に指定する。Render の proxy / 候補ペア指定は未対応エラーになる。
- 階層の深さ範囲は 0–64。対象ごとの描画コマンド上限は 1–4096、両対象合計で最大8192。複数内容を有効にしても対象内で同じ上限を共有する。
- 選択対象のみ、深度テストも指定できる。複数選択時は先頭の有効なEntity、複数Collider時は既存同期順の最初の有効Colliderを使う。
- Render の既定方式は線形のまま。木を見る専用実行では、起動前に `FBZZ_MESH_SPATIAL_MODE=bvh` または `validate` を指定する。`auto` では個体数などにより木が使われないことがある。表示をONにしただけで本番方式を変更しない。
- Physics はPlayの実ステップ後に取得する。Pause中の値は最後に完了したステップのもので、表示に sequence を示す。sleep等で照会が省略された場合は候補を未取得と表示し、前の候補を再利用しない。
- 上限、選択検索の打ち切り、モード、数値fallback、Scene / View不一致は状態欄に表示する。検索の打ち切りを「対象なし」と説明しない。

Render の緑は観測した生存内部ノード、赤は観測した除外、黄は通過した候補葉。照会されなかったノードは partition 色（静的青／動的橙）。除外された親の子に除外状態を推測して付けない。Physics 階層は灰、proxy は静的青／動的橙、候補ペア線は黄。Physics の探索／除外ノード状態は取得していない。

公開Operatorは `render.set_spatial_debug`、読み取りは `render.get_spatial_debug`。設定フィールドは `target` (`render` / `physics`)、`enabled`、`hierarchy`、`proxy_bounds`、`candidate_pairs`、`min_depth`、`max_depth`、`max_draw_commands`、`selected_only`、`depth_test`。読み取り結果には設定、状態文字列、実際の前回Scene View描画コマンド数、runtime状態が含まれる。これはEditorの一時診断設定であり、Scene / Materialへ保存しない。

## 所有権と寿命

`Graphics/Renderer/RenderBvhDebug.hpp` は thread-safe な要求と不変値snapshotを提供する。要求をScene寿命、出力RTのIDと世代に限定し、別Viewによる上書きを防ぐ。UIはScene / Viewを検証し、ResourceManagerの時計で2フレーム以内の値だけを描く。抽出フレームの時計と混同しない。

`Physics/World` の診断setter/getterは物理所有スレッド専用。Editorは呼ばない。`PhysicsSystem` のthread-safeなbridgeが要求を集約し、EndSceneSync後にEntity世代と現存ColliderHandleを解決してStep前へ渡す。Step後の値だけを要求revisionとScene寿命を再検証して公開する。World epochはWorld再生成後の同じhandle値と区別する。

Play停止、Scene寿命変更、Editor終了で公開値を無効化し、古い処理からの再公開も拒否する。snapshotにはScene、Collider、Body、木の所有者へのポインタを含めない。保持した古いsnapshot自体は値として安全だが、新しいScene表示には使わない。

UIはsnapshotから `ScriptDebugDrawCommand` の値だけを生成し、Scene View固有のspanとして同期RenderSystemへ渡す。既存DebugDrawのcapture/replayを使い、Script GizmosをOFFにしても診断表示は有効にできる。共有Sceneスクリプトキューへ積まず、Game Viewにも流さない。

## 計算量と性能測定

Nを本番のproxy／mesh源数、Vを本番照会が訪れたノード数、Bを指定表示上限（最大4096）、Hを深さ（最大64）とする。元のBVH構築・更新・照会アルゴリズムは置換しない。照会の最悪はO(N)であり、無条件にO(log N)とはしない。

OFF時は診断木の追加走査、bounds照会、全件コピー、snapshot確保を行わない。要求境界の有効フラグ／同一性確認に加え、Renderの既存BVH探索内には無効な観測ポインタへの条件分岐が残る（追加分岐はO(V)）。これを全体として定数時間とは説明しない。既定の線形方式ではそのBVH探索自体を使用しない。分岐の実時間への影響は未計測であり、無償と断言しない。

Render の階層収集は出力数だけでなく検査数もBで制限する。選択検索は別途4096源のprefixまでで、見つからなければ打ち切りを明示する。選択経路の祖先検査もBに含む。収集メモリはO(B)。本番照会の観測は既存の前方preorder探索に単調cursorを合わせ、収集ノードごとの二分検索をしない。診断によって本番照会自体をBで打ち切ることはない。

Physics はnode／proxy／pairの出力を各4096以下に制限し、各走査の検査数もそれぞれ65536までとする。World側の選択探索とhandle解決も別々に65536まで。全体を無制限に走査してから先頭だけをコピーする設計ではない。打ち切り時は部分snapshotとして明示する。pair数が大きいシーンでも診断のためだけに全候補を複製しない。本番候補配列自体のサイズは既存アルゴリズムのもの。

ON時はsnapshot作成、mutex／共有所有、線の構築とGPU描画の追加コストがある。性能測定時は両対象をOFFにする。デバッグ表示の件数が少ないことを本番性能改善の証拠にしない。

## 検証境界

Graphicsテストは選択世代0、深さ／検査／出力上限、未観測子、候補一致、fallback、Scene／View／revisionと保持値の寿命を扱う。Physicsテストは無効時の追加AABB読取りなし、BVH／brute-force／sleep、上限、削除とID再利用、World再生成を扱う。Engine bridgeとEditorのテストは所有境界、要求変更、有限bounds、frame age、複合表示上限、OFFを扱う。

実画面の成否、実行した集約テストと制限は今回の検証報告を参照する。コンパイルのみで実描画を検証済みとは扱わない。
