<!-- @file    technical-article-plan.md -->
<!-- @brief   FBZZ Engine の5つの実装群から題材を選ぶ技術連載の計画。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-20 -->

# FBZZ Engine の実装から選ぶ技術連載

Qiita などに向けて、**5群から全22本**を選ぶ。1本では一つの設計判断を取り上げ、実装をたどって、何を守るための設計かをテスト・図・比較のいずれかで示す。具体的な例で説明できる技術を優先する。

| 群 | 記事で扱う責務 | 回 | 本数 |
| --- | --- | --- | ---: |
| Editor | プレビュー、選択と編集、制作データの操作、MCP 連携 | #04・#17–#22 | 7 |
| FBZZCore | 一時メモリの管理、計測の記録とその寿命 | #05・#06 | 2 |
| FBZZMath | 座標・回転の規約、交差判定とデータのまとめ方 | #01・#07 | 2 |
| FBZZPhysics | 衝突判定、剛体の接触応答、位置拘束、キャラクターの骨格への接続 | #12–#16 | 5 |
| FBZZGraphics | 描画入力、実行計画、命令記録、GPU 資源、描画方式 | #02・#03・#08–#11 | 6 |
| **合計** | | | **22** |

各回の所属は、主に説明する設計で決める。**MaterialPreview は Editor の題材**として、専用の形状・カメラ・照明と再描画の条件を扱う。**足 IK と Ragdoll は FBZZPhysics の応用回**として、Engine が骨の姿勢や接地目標を扱い、FBZZPhysics が XPBD の関節と駆動を解く境界を示す。 [MaterialPreview.cpp](../Projects/Editor/src/Panels/MaterialPreview.cpp) · [Graphics の境界](design/graphics-library.md) · [Ragdoll の責務](design/ragdoll-api.md)

## 依存関係順の公開計画

番号がそのまま執筆・公開順。前提回には、本文で使う概念を説明した回を記す。最初の4本は、同じ球の座標変換、描画入力、GPU 資源の寿命まで先に説明し、Editor での材質プレビューへつなぐ。

| 回 | 群・仮タイトル | 設計の問いと確かめ方 | 前提回 |
| ---: | --- | --- | --- |
| 01 | **FBZZMath** — 自作 Math の TRS と Quaternion | **変換と補間をどの規約で揃える？** 行ベクトルで回転・平行移動の順番を変えたときの点の位置を描く。姿勢補間は Quaternion の符号の扱いを Slerp のテストから取り上げる。 [Matrix4.cpp](../Projects/Math/src/Matrix4.cpp) · [QuaternionTests.cpp](../Projects/Tests/Math/Auto/QuaternionTests.cpp) | — |
| 02 | **FBZZGraphics** — `RenderScene` を境界にした描画ライブラリ | **Scene の構造から描画をどう独立させる？** Engine が解決した姿勢・メッシュ・材質を、値と借用ハンドルにして Graphics へ渡す。Engine をリンクせずに三角形を描いて読み戻すテストで境界を示す。 [RenderScene.hpp](../Projects/Graphics/include/Graphics/Renderer/RenderScene.hpp) · [GraphicsStandaloneTests.cpp](../Projects/Tests/Graphics/Auto/GraphicsStandaloneTests.cpp) · [分離の設計](design/graphics-library.md) | #01 |
| 03 | **FBZZGraphics** — Render Thread と GPU 資源の寿命 | **ゲーム更新を続けながら、描画入力と資源をいつまで保持する？** 更新を担うスレッド、命令を記録するスレッド、GPU の3段の時系列を描く。提出元で入力を解放する既存テストと、フェンスで資源を退役させる境界を読む。 [RenderThread.cpp](../Projects/Graphics/src/Renderer/RenderThread.cpp) · [RenderThreadTests.cpp](../Projects/Tests/Graphics/Auto/RenderThreadTests.cpp) · [シェーダー差し替えの寿命](design/shader-reload-lifetime.md) | #02 |
| 04 | **Editor** — MaterialPreview の独立描画と二段階キャッシュ | **同じ材質をどう観察し、どんな変更で描き直す？** Editor が専用の形状・カメラ・照明を組み立て、専用ターゲットへ描いて ImGui に出す。視点の回転と、ハンドルを維持したままのテクスチャ再読込みで、GPU 入力と描画済み画像の失効条件を比べる。 [MaterialPreview.cpp](../Projects/Editor/src/Panels/MaterialPreview.cpp) · [MaterialPreviewCacheTests.cpp](../Projects/Tests/Editor/Auto/MaterialPreviewCacheTests.cpp) | #03 |
| 05 | **FBZZCore** — フレーム単位の一時メモリ確保 | **一時データの寿命をフレーム末尾で揃えると、解放をどうまとめられる？** `FrameAllocator` と `LinearAllocator` を使い、確保・アラインメント・フレーム末尾のリセットを図にする。次のフレームで容量を再利用できることをテストで確認する。 [FrameAllocator.cpp](../Projects/Core/src/Memory/FrameAllocator.cpp) · [LinearAllocator.cpp](../Projects/Core/src/Memory/LinearAllocator.cpp) · [FrameAllocatorTests.cpp](../Projects/Tests/Core/Auto/Memory/FrameAllocatorTests.cpp) | — |
| 06 | **FBZZCore** — スコープ計測とフレームタイムライン | **入れ子の処理やスレッドをまたぐ待ちを、どうフレームに結びつける？** RAII スコープと記録トークンを追い、親を含む時間・関数自身の時間・フレーム間隔の意味を図にする。タイムラインはフレーム番号による関連付けと、記録容量が尽きた場合を取り上げる。 [ProfileScope.hpp](../Projects/Core/include/Core/Profiler/ProfileScope.hpp) · [FrameTimeline.hpp](../Projects/Core/include/Core/Profiler/FrameTimeline.hpp) · [ProfileRecorderTests.cpp](../Projects/Tests/Core/Auto/Profiler/ProfileRecorderTests.cpp) | #03 |
| 07 | **FBZZMath** — レイと視錐台の判定 | **マウスの先にある面と、カメラに見える球をどう判定する？** レイと三角形の交差、視錐台を構成する平面と球の距離を図にする。球の一括判定では4個ずつ SIMD で処理し、単体判定と結果を照合する。 [Ray.cpp](../Projects/Math/src/Ray.cpp) · [Frustum.cpp](../Projects/Math/src/Frustum.cpp) · [FrustumTests.cpp](../Projects/Tests/Math/Auto/FrustumTests.cpp) | #01 |
| 08 | **FBZZGraphics** — RenderGraph の依存解決とパスの省略 | **読み書きの宣言から、順序と実行の要否をどう決める？** 消費側を先に登録した例と、出力につながらないパスの例を使う。中間画像の観察例として Editor の Render Pass Viewer から、同じ HDR が上書きされる前後を見る。 [RenderGraph の設計](design/render-graph.md) · [RenderGraphTests.cpp](../Projects/Tests/Engine/Auto/RenderGraphTests.cpp) · [Render Pass Viewer](design/render-pass-viewer.md) | #02・#06 |
| 09 | **FBZZGraphics** — Forward／Deferred と光源の扱い | **材質ごとの見た目を保ちながら、どのパスで照明を計算する？** 標準の不透明材質・独自シェーダーの不透明材質・半透明材質が通る経路を一つのビューで比べる。クラスタードライティングとカスケード影は、照明に渡す情報をどう絞るかに焦点を当てる。 [材質の振り分け](../Projects/Graphics/include/Graphics/Renderer/GeometryRoute.hpp) · [ClusterLightPass.cpp](../Projects/Graphics/src/Passes/Geometry/ClusterLightPass.cpp) · [ShadowPass.cpp](../Projects/Graphics/src/Passes/Geometry/ShadowPass.cpp) | #07・#08 |
| 10 | **FBZZGraphics** — 画面空間の情報と時間履歴 | **今ある画面をどこまで反射やアンチエイリアシングに使える？** SSR が画面外を参照できない場面と、TAA がモーションベクターで履歴を再投影できる条件を図にする。再投影した位置と現在の画素を比較する。 [SSRPass.cpp](../Projects/Graphics/src/Passes/PostProcess/SSRPass.cpp) · [TAAPass.cpp](../Projects/Graphics/src/Passes/PostProcess/TAAPass.cpp) | #09 |
| 11 | **FBZZGraphics** — Hi-Z・間接描画・Meshlet | **GPU に描画量の判断を任せるのはどんな場面？** 深度階層で候補を絞り、GPU で作った描画引数を間接描画へ渡す経路を主題にする。Meshlet と Bindless の役割を添え、全体が見える高密度メッシュと視錐台から一部が外れる場合で、選ばれる経路を比べる。 [遮蔽カリング](design/occlusion-culling.md) · [GPU 駆動描画](design/gpu-driven-geometry.md) · [Bindless](design/bindless.md) | #10 |
| 12 | **FBZZPhysics** — 衝突候補を絞る BVH の差分更新 | **動いたコライダーが一部なら、木をどこまで作り直す？** 変更した葉と祖先を refit する範囲と再構築の条件を、1個の移動と多数の移動で比較する。総当たりと候補集合が一致するテストを根拠にする。 [BVH 差分更新の設計](design/broadphase-incremental.md) · [BroadPhaseIncrementalTests.cpp](../Projects/Tests/Physics/Auto/BroadPhaseIncrementalTests.cpp) | #07 |
| 13 | **FBZZPhysics** — GJK から EPA へ、凸形状の衝突を求める | **当たったかどうかから、めり込みの方向と深さをどう得る？** 2つの凸形状を題材に Support 点と単体を描き、GJK の交差判定から EPA に何を引き渡すかを示す。 [GJK.cpp](../Projects/Physics/src/GJK.cpp) · [EPA.cpp](../Projects/Physics/src/EPA.cpp) | #12 |
| 14 | **FBZZPhysics** — 接触応答と高速な球のすり抜け対策 | **固定時間刻みの中で、いつ接触し、どの速度を更新する？** 逐次インパルスと接触キャッシュを一つの接触から追う。高速な球では CCD が最初の接触時刻から移動を制限する様子を時系列にする。 [PhysicsSolver.cpp](../Projects/Physics/src/PhysicsSolver.cpp) · [ContactCache.cpp](../Projects/Physics/src/ContactCache.cpp) · [CCDSolver.cpp](../Projects/Physics/src/CCDSolver.cpp) | #13 |
| 15 | **FBZZPhysics** — XPBD と布の位置拘束 | **つながった形を保ちつつ、どの拘束を柔らかくできる？** 片端を固定した布を題材に伸びと曲げを分け、コンプライアンスを変えたときの位置補正を1反復分だけ追う。 [XPBDSolver.cpp](../Projects/Physics/src/XPBDSolver.cpp) · [ClothSolver.cpp](../Projects/Physics/src/Cloth/ClothSolver.cpp) · [布の設計](design/cloth.md) | #14 |
| 16 | **FBZZPhysics** — 足 IK と Ragdoll の接続 | **足の接地目標を、物理で動く骨格へどう渡す？** Engine の `IKSystem` が接地を求め、有効な物理足の形状があれば足裏の位置から目標を組み立てて `RagdollSystem` へ渡す。Passive とアニメーション姿勢を目標とする Active を紹介し、FBZZPhysics の XPBD 関節・駆動が解いた姿勢を Engine が骨へ戻すまで、一つの脚で追う。 [足 IK の接地契約](design/foot-ik-tuning.md) · [IKSystem.cpp](../Projects/Engine/src/Scene/Systems/IKSystem.cpp#L567) · [RagdollSystem.cpp](../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp) · [XPBDBodyDrive.cpp](../Projects/Physics/src/XPBDBodyDrive.cpp) · [物理足形状のテスト](../Projects/Tests/Engine/Auto/FootIKTests.cpp#L110) | #07・#15 |
| 17 | **Editor** — 操作の登録・複数編集・Undo | **画面や呼び出し口が増えても、操作条件と履歴をどう揃える？** メニューとパレットが同じ登録操作を呼ぶ経路を追う。Inspector の複数編集を使い、変更した軸と一件の Undo をどう確定するかも示す。 [OperatorRegistry.cpp](../Projects/Editor/src/Op/OperatorRegistry.cpp) · [InspectorMultiEdit.cpp](../Projects/Editor/src/Panels/Inspector/InspectorMultiEdit.cpp) · [UndoStack.cpp](../Projects/Editor/src/Util/UndoStack.cpp) | — |
| 18 | **Editor** — Viewport の選択と親子ギズモ | **クリックで拾った対象に、どの変換を適用する？** 選択のレイとメッシュのローカル BVH を追い、親子をまとめて選んだ例では操作対象をルートに絞る。位置を編集する対象を見つける処理と、変換を反映する処理を1操作の前後で示す。 [ViewportPicking.cpp](../Projects/Editor/src/Panels/Viewport/ViewportPicking.cpp) · [ViewportSceneGizmos.cpp](../Projects/Editor/src/Panels/Viewport/ViewportSceneGizmos.cpp) · [選択とスナップの設計](design/editor-idle-work.md) | #12・#17 |
| 19 | **Editor** — Asset Browser の遅延サムネイル | **大きな一覧で、どの画像をいつ作る？** 画面に入った画像を題材に、デコードをワーカーに回し、受理した結果で GPU テクスチャを作るまでを追う。3D 材質のサムネイルは #04 のプレビュー処理を使う例として扱う。 [サムネイルの設計](design/asset-browser-thumbnails.md) · [AssetBrowserThumbnail.cpp](../Projects/Editor/src/Panels/AssetBrowser/AssetBrowserThumbnail.cpp) · [AssetBrowserThumbnailTests.cpp](../Projects/Tests/Editor/Auto/AssetBrowserThumbnailTests.cpp) | #04 |
| 20 | **Editor** — Prefab の三者マージ | **元アセットと個別編集が同時に変わったとき、どう更新する？** 前回のソース、現在のインスタンス、新しいソースを図にする。親変更が競合する例と、プレビューのあとにデータが変わって適用を拒否する既存テストを読む。 [PrefabStructureMerge.cpp](../Projects/Editor/src/Util/PrefabStructureMerge.cpp) · [Prefab の設計](design/prefab-structural-merge.md) · [PrefabStructureMergeTests.cpp](../Projects/Tests/Editor/Auto/PrefabStructureMergeTests.cpp) | #17 |
| 21 | **Editor** — ノードグラフの自動配置と部分複製 | **同じ接続を保ったまま、ノードをどう並べ、どう複製する？** 分岐と合流のあるグラフで、深さから列を割り当てる方法と、複製したノード ID へのリンクの張り直しを示す。 [GraphLayoutAlgo.cpp](../Projects/Editor/src/GraphEditor/GraphLayoutAlgo.cpp) · [GraphSubgraphOps.cpp](../Projects/Editor/src/GraphEditor/GraphSubgraphOps.cpp) · [GraphLayoutTests.cpp](../Projects/Tests/Editor/Auto/GraphLayoutTests.cpp) | — |
| 22 | **Editor** — Editor MCP と Command Bus | **外部ツールから登録済みの操作をどう呼び、結果を確かめる？** stdio MCP → Named Pipe → Editor の Command Bus → `OperatorRegistry` を、操作の照会と呼び出しで1往復追う。read／dry-run／write の違いと、Playtest の `scenario_run` → `scenario_status` で受付・実行結果を区別する仕組みを示す。 [Editor MCP の構成](../Projects/EditorMcp/README.md) · [Editor 直接 API](design/EditorDirectApi.md) · [busClient.ts](../Projects/EditorMcp/src/busClient.ts) · [PlaytestTools.ts](../Projects/EditorMcp/src/Tools/PlaytestTools.ts) | #17 |

## 各回の書き方

困る場面を一つ示し、採用した設計の理由を一文にする。実装では入力から出力まで一つの経路を追い、条件を一つ変えて結果を確かめる。たとえば #04 では「カメラだけを回す」と「同じテクスチャを再読込みする」の2操作で、キャッシュの役割を説明できる。関連技術は主題に必要な範囲を添え、コードへのリンクは確認に使ったコミットに固定する。
