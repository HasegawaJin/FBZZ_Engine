<!-- @file    technical-article-plan.md -->
<!-- @brief   FBZZ Engine の設計判断をコードとテストで読み解き、Qiita の全23回にまとめるための執筆計画。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-20 -->

# FBZZ Engine を読み解いて書く Qiita 連載計画

最初の記事では、球の表面に印を一つ付ける。その球を拡大し、回し、横に動かしたら、印はどこに来るだろうか。手で予想してから、FBZZMath の実装とテストで答え合わせする。続く3本では、その球の情報を誰が描画側に渡し、いつまで保持し、材質プレビューをいつ描き直すかを追う。第5回はここまでをつなぎ、Engine・Graphics・Editor が一つの球をどう分担して描くかをまとめる。

全23回の公開順と前提回を以下に示す。各回の読者は、C++ の短い関数を読めて、FBZZ Engine には初めて触れる人を想定する。記事に載せる図や比較は各回に一つ用意し、そこで読者が予想できる結果を決めてからコードを選ぶ。

| 公開順 | 扱う部分 | 書き終えたときに自分で説明できること |
| --- | --- | --- |
| [#01–05　球がプレビューに映るまで](#球がプレビューに映るまで) | Math → Graphics → Editor | 頂点の座標、描画入力、GPU 資源、表示画像を追い、Engine・Graphics・Editor の責務を説明できる |
| [#06–12　1フレームを組み立てる](#1フレームを組み立てる) | Core・Math・Graphics | 一時データがいつ消え、どの描画をどの順序で実行し、何を省けるか |
| [#13–17　形状の重なりから足の接地まで](#形状の重なりから足の接地まで) | Physics・Engine | 衝突候補、接触点、拘束、骨の姿勢は、それぞれ誰が何に使うか |
| [#18–23　エディターの操作を追う](#エディターの操作を追う) | Editor・Editor MCP | 一つの編集について、対象を決める処理、値を変える処理、結果を確かめる処理がどこにあるか |

5群の内訳は FBZZMath 2本、FBZZCore 2本、FBZZGraphics 7本、FBZZPhysics 5本、Editor 7本。Engine は Scene の情報を Graphics や Physics が扱える入力に変え、必要な結果を Scene に戻す。#02 のメッシュと #17 の足を読むと、この役割を往路と復路の両方から見られる。 [ライブラリの責務と依存方向](design/graphics-library.md)

## 各回を Qiita の下書きにする手順

1. **読者が予想できる問いで始める。** 「TRS の実装を紹介する」よりも「球を2倍にしたら、球を置く位置まで2倍になる？」と書く。題材を知らない読者にも、結果を二通り想像できる。
2. **コードを開く前に入力と期待値を書く。** 既存テストなら準備部分と最後の `EXPECT` を先に読む。その間をつなぐ関数へ進み、結果を決める式か分岐を一か所抜き出す。
3. **設計理由を比較で説明する。** 「こう書くと便利」で終えず、「もし順序・所有者・条件を一つ変えたら、どの値や見た目が変わるか」を図や数値で示す。設計文書に記された採用理由と、自分で考えた別案にはそれぞれ出所を添える。
4. **問いに答えて締める。** 下書きの見出しは「起きる現象 → 自分の予想 → 実装の抜粋 → 比較結果 → この設計を使える条件」。読んだテストの期待値、実行して得た結果、自分の手計算にはそう明記する。公開用のコードリンクは確認したコミットに固定する。本計画の読解基準は `abbce8c8e6a8`。

画像と原稿は `FBZZEngine_Qiita` にそろえる。第4回なら `FBZZEngine#04-Water-Preview.gif` のように、素材名は `FBZZEngine#回番号2桁-内容.拡張子` とする。`FBZZEngine#04-Local.md` は同じフォルダーの画像を相対参照し、リンク内の `#` は `%23` と書く。`FBZZEngine#04-Qiita.md` は同じ本文にアップロード済みの画像URLを入れる。

## 球がプレビューに映るまで

### #01　FBZZEngineの3D座標変換！（FBZZMath・前提なし）

**最初の下書きの作例。** 球の中心を原点、印のローカル座標を `(1, 0, 0)` とする。球自体を2倍にし、Y 軸まわりに90度回し、中心を X 方向に5移したい。テスト `TRSAppliesScaleThenRotationThenTranslation` の入力をそのまま使う。 [入力と期待値](../Projects/Tests/Math/Auto/Matrix4Tests.cpp#L80)

| 紙に描く順番 | 印の座標 | 何が起きたか |
| --- | --- | --- |
| 最初 | `(1, 0, 0)` | 中心から右へ1 |
| 拡大 S | `(2, 0, 0)` | 中心からの距離が2倍 |
| 回転 R | `(0, 0, -2)` | 中心を軸に印が回る |
| 平行移動 T | `(5, 0, -2)` | 球全体が右へ5移る |

**コードと図をつなぐ。** 数式は `p_world = T * R * S * p_local`。FBZZMath は列ベクトルを行列の右に掛けるので、点には右側の S から効く。行列の要素はメモリ上で行優先に並ぶ。`Matrix4::TRS` の本体では回転行列の各列にスケールを掛け、第4列に平行移動を置く。これで「球の大きさを変えても、指定した中心は X=5 に置く」という結果を保てる。 [積と格納の規約](../Projects/Math/include/Math/Matrix4.hpp) · [TRS の実装](../Projects/Math/src/Matrix4.cpp#L38)

テストは行列で変換した座標を、拡大・回転・平行移動を一つずつ適用した座標と比べている。下の2行を記事に引けば、読者も表の各行が式のどこに対応するか指で追える。 [テストの比較式](../Projects/Tests/Math/Auto/Matrix4Tests.cpp#L89)

```cpp
EXPECT_VEC3_NEAR(TransformPoint(math::Matrix4::TRS(t, r, s), point),
                 t + (r * (point * 2.0f)), testkit::kLooseTolerance);
```

**記事に載せる比較。** 拡大の後で、平行移動と回転だけを交換してみる。印は `(2, 0, 0) → (7, 0, 0) → (0, 0, -7)` になる。これは同じ入力で順序を変えた手計算例。二つの終点に加えて球の中心も描くと、後者では置き場所まで原点のまわりを回ったことが伝わる。

**姿勢補間の小欄。** 同じ姿勢を表す単位 Quaternion `q` と `-q` の中間はどう見えるべきか。`Slerp` の `dot < 0` で終点の符号を反転する行と、中間でも元の姿勢を保つ期待値を並べる。「異なる4成分が同じ姿勢を表せる」ことがその一行の理由になる。TRS の適用順とこの符号のテストは、2026-11-20 に既存の Release ビルドで2件とも合格した。 [Slerp の分岐](../Projects/Math/src/Quaternion.cpp#L133) · [符号を反転させるテスト](../Projects/Tests/Math/Auto/QuaternionTests.cpp#L147)

### #02　FBZZEngineの描画データ抽出！（FBZZGraphics・前提 #01）

Scene 上の球を1個選び、`ExtractRenderSceneGeometry` から `RenderScene` の `objects` と `items` を追う。前者にはワールド行列や境界、後者には描画する頂点・インデックスのバッファを識別するハンドルが入る。Scene の探索を Engine が終えて渡せば、Graphics はゲーム側のオブジェクト構造から独立して描ける。 [抽出の入口](../Projects/Engine/src/Scene/Systems/RenderSceneExtractor.cpp#L1886) · [描画入力の型](../Projects/Graphics/include/Graphics/Renderer/RenderScene.hpp)

**記事の図**は「元のメッシュ → 抽出した値とハンドル → 描画」の3箱。抽出後に元のメッシュを書き換えたら、コピー済みのインデックス数36はどうなるかをテストから埋める。別の単体描画テストでは Engine をリンクせず三角形を提出し、画像中央と隅の色を読む。この二つがそれぞれ、値を渡す境界とライブラリの独立性の根拠になる。 [抽出後に元データを変える例](../Projects/Tests/Engine/Auto/RenderSceneExtractorTests.cpp#L630) · [三角形の描画と読戻し](../Projects/Tests/Graphics/Auto/GraphicsStandaloneTests.cpp#L383)

### #03　FBZZEngineのGPUリソース管理！（FBZZGraphics・前提 #02）

ゲーム更新、描画命令を記録する CPU スレッド、GPU の3本の時間軸を描く。`RenderThread::Submit` でフレームの所有権が移り、描画側は `Execute` とフレームの破棄を終えてから完了番号を通知する。一方、記録した命令が使う GPU 資源の解放には、GPU がどこまで実行したかを示すフェンス値が要る。 [所有権を渡す箇所](../Projects/Graphics/src/Renderer/RenderThread.cpp#L67) · [GPU 資源の解放判定](../Projects/Graphics/src/Renderer/Platform/DX12/DX12Context.cpp#L1333) · [描画スレッドの設計](design/render-thread.md)

**図に置く証拠。** 提出元が配列を空にしても、テストの描画側は `{2, 3, 5}` を読む。図に「元配列を消せる」「CPU 側のフレームが終わる」「GPU 資源を解放できる」の3点を打つ。シェーダーを古い命令が使っている場合にも、この区別が利く。 [所有入力のテスト](../Projects/Tests/Graphics/Auto/RenderThreadTests.cpp#L118) · [シェーダー差し替えの契約](design/shader-reload-lifetime.md)

### #04　FBZZEngineのマテリアルプレビュー！（Editor・前提 #03）

Editor は専用の球・カメラ・照明で材質を画面外の画像に描き、ImGui に表示する。`EnsureGpuData` の比較対象と `EnsurePreviewFrame` の比較対象を順に読む。材質を GPU に渡す準備と、ある視点から見た画像では、変化する理由が違う。 [二つの判定が並ぶ実装](../Projects/Editor/src/Panels/MaterialPreview.cpp#L614) · [キャッシュのテスト](../Projects/Tests/Editor/Auto/MaterialPreviewCacheTests.cpp#L175)

**記事に載せる予測表。** 静止した材質で一度描画が成功したあとを考える。表の下に、ハンドルが同じでもテクスチャの中身が変わり得る具体例を添える。

| 次の操作 | 材質の GPU 入力 | プレビュー画像 |
| --- | --- | --- |
| 同じ状態のまま表示 | 再利用 | 再利用 |
| 視点だけ回す | 再利用 | 描き直す |
| 参照中のテクスチャを同じハンドルのまま再読込み | 再構築 | 描き直す |

### #05　FBZZEngineの描画パイプライン！（FBZZGraphics・前提 #01–04）

第1〜4回をつなぎ、シーン入力から画素までを俯瞰する。Engine が姿勢・材質・ハンドルを抽出し、Graphics がビュー別の可視判定、材質の振り分け、影・照明・画面効果を担う。RenderPipeline がビューの設定に応じてパスを集め、RenderGraph は各パスが申告した資源の読み書きと必要な出力から実行計画を作る。幾何・水面や粒子・レイを使う経路・時間方向の履歴・DX12 実行・計測の位置を示す。Editor の材質プレビューは独自の描画先から同じ描画 API を利用する。 [分離で保証すること](design/graphics-library.md) · [ビューのパス構成](../Projects/Graphics/src/Pipeline/ViewPipeline.cpp) · [グラフの契約](design/render-graph.md)

**記事で答える問い。** 影を使うパスを先に登録したら何が起きるか。`Present` が読む `Shadow` の生成側を後から登録するテストで、`ShadowMap → Present` になる理由と、出力に届かない省略可能なパスを外す条件を読む。第2回の三角形をシーン入力を経ない直接描画の例として再掲し、最後に CPU 完了と GPU フェンスの違いまで戻る。#09 は依存解決と中間画像の観察を詳説し、#10 は Forward / Deferred、#11 は反射と履歴、#12 は遮蔽カリングを掘り下げる。 [順序の実測テスト](../Projects/Tests/Engine/Auto/RenderGraphTests.cpp#L350) · [最終出力と枝の省略](../Projects/Tests/Engine/Auto/RenderGraphTests.cpp#L230)

## 1フレームを組み立てる

### #06　FBZZEngineのフレームアロケータ！（FBZZCore・前提なし）

そのフレームで使い切る作業領域を二つ確保する。`LinearAllocator::Allocate` は各領域の先頭を指定の境界に揃えて使用位置を進め、`FrameAllocator::EndFrame` は使用位置をリセットする。一斉に寿命が切れる用途なら、個々の解放を管理する必要がなくなる。 [確保位置の計算](../Projects/Core/src/Memory/LinearAllocator.cpp#L69) · [フレーム末尾のリセット](../Projects/Core/src/Memory/FrameAllocator.cpp#L26)

**記事の図**には、一本の領域に使用中の部分・位置合わせの空き・残り容量を描く。128バイトを3フレーム続けて確保できるテストを読み、#03 の描画スレッドがまだ読むデータをここに置いた場合、どの時点で上書きされ得るかも答える。 [容量を再利用するテスト](../Projects/Tests/Core/Auto/Memory/FrameAllocatorTests.cpp#L32)

### #07　FBZZEngineのCPUプロファイラ！（FBZZCore・前提 #03）

`ProfileScope` はスコープに入ると記録トークンを受け取り、抜けると対応する記録を閉じる。既存テストのフレーム42を使い、親を0〜5 ms、子を1〜3 ms の帯で描く。親の区間は5 ms、子を除いた親自身は3 ms。親と子を足すと二重に数える区間が図から読める。 [スコープの開始と終了](../Projects/Core/include/Core/Profiler/ProfileScope.hpp) · [時刻を固定したテスト](../Projects/Tests/Core/Auto/Profiler/ProfileRecorderTests.cpp#L25)

**記事で一段進む問い。** 描画スレッドの待ちを同じ図に足したら、帯の合計からフレーム間隔が出せるだろうか。フレーム番号で対応づけ、隣り合うフレームの開始時刻から実時間を読む。イベント容量を超えたときは、落ちた記録の件数も見る。 [タイムラインの契約](../Projects/Core/include/Core/Profiler/FrameTimeline.hpp) · [並列区間・フレーム境界・欠落のテスト](../Projects/Tests/Core/Auto/Profiler/ProfileRecorderTests.cpp#L633)

### #08　FBZZEngineのレイ判定と視錐台カリング！（FBZZMath・前提 #01）

前者は1本のレイが三角形のどこに届くか、後者は球全体がカメラの見える範囲（視錐台）の外に出たかを問う。`IntersectTriangle` の戻り値と、`IntersectsSpheres` が平面への符号付き距離を `-radius` と比べる式を読む。球の中心が外でも、表面が視野にかかれば描画候補に残せる。 [レイと三角形](../Projects/Math/src/Ray.cpp#L41) · [球と視錐台](../Projects/Math/src/Frustum.cpp#L50) · [中心が外にある例](../Projects/Tests/Math/Auto/FrustumTests.cpp#L116)

**記事の図**に視錐台の側面を一本引き、境界をまたぐ球を描く。球が5個のとき、最初の4個を SIMD（複数の値を一命令で計算する方式）、端数の1個を単体経路で処理しても、可視判定が一個ずつ計算した結果と揃う理由を説明する。 [一括判定との照合](../Projects/Tests/Math/Auto/ScalarParityTests.cpp#L203)

### #09　FBZZEngineのレンダーグラフ！（FBZZGraphics・前提 #02・#07）

描画パスを「どの画像を読み、どの画像を書く工程か」で表す。既存テストでは `Present` を先に、そこで読む `Shadow` を作る `ShadowMap` を後に登録する。`SetOutputs` で必要な出力を指定し、`Plan` が返す順序を予想する。作る側が先に動き、最終出力へ届かない省略可能な枝は実行から外れる。 [読み書きと計画の実装](../Projects/Graphics/include/Graphics/Renderer/RenderGraph.hpp) · [登録順を逆にしたテスト](../Projects/Tests/Engine/Auto/RenderGraphTests.cpp#L350) · [孤立した枝のテスト](../Projects/Tests/Engine/Auto/RenderGraphTests.cpp#L230)

**記事の図**には `ShadowMap → Shadow → Present → Output` と孤立した枝を描く。応用として、同じ HDR 画像を別パスが上書きする例で、Render Pass Viewer がどのパスの直後に画像を採ると途中結果を見られるかを説明する。 [RenderGraph の設計](design/render-graph.md) · [Viewer の取得タイミング](design/render-pass-viewer.md)

### #10　FBZZEngineのディファード描画！（FBZZGraphics・前提 #08・#09）

Deferred では、面の材質情報を GBuffer に保存して後段で照明を計算する。Forward では面を描くときに照明を計算する。`ResolveGeometryRoute` は半透明か、標準シェーダーと同じ情報で描けるか、追加の材質特性があるかをスロット単位で判定する。これで独自シェーダーの見た目を保ち、同じ面を二重に描くことも防げる。 [振り分けを決める関数](../Projects/Graphics/include/Graphics/Renderer/GeometryRoute.hpp#L30) · [材質別のテスト](../Projects/Tests/Engine/Auto/GeometryRouteTests.cpp#L59)

**記事の比較表**は Deferred ビューに標準の不透明 PBR、独自の不透明材質、半透明材質を置き、順に `GBuffer`、`ForwardOpaque`、`ForwardTransparent` と埋める。光源の話は、空間の区画ごとに参照するライトを絞る方法と、視距離の区間ごとに影を描くカスケードの図を、この表に添える。 [クラスタのライト](../Projects/Graphics/src/Passes/Geometry/ClusterLightPass.cpp) · [影のパス](../Projects/Graphics/src/Passes/Geometry/ShadowPass.cpp)

### #11　FBZZEngineのSSRとTAA！（FBZZGraphics・前提 #10）

床に物体が映る場面を使う。画面空間反射（SSR）は現在の画面の深度から反射先を探すため、映したい物体が画面外に出ると、その表面をこの方法で参照できない。時間方向のアンチエイリアシング（TAA）は、前フレームの同じ表面を探して色を混ぜる。`TAA.hlsl` では有効な速度があると `previousUV = uv - velocity.xy` で前の画素位置を求める。 [SSR の入力](../Projects/Graphics/src/Passes/PostProcess/SSRPass.cpp#L42) · [TAA のパス](../Projects/Graphics/src/Passes/PostProcess/TAAPass.cpp) · [再投影と履歴深度の照合](../Assets/Shaders/PostProcess/AntiAliasing/TAA.hlsl#L199)

**記事の図**は現在と前フレームの2枚。物体が横に動いたとき、表面上の画素からは前の位置へ矢印を引き、新しく露出した背景では深度が合わず履歴を採用できない例を描く。SSR が使える範囲と、TAA が過去の色を信頼できる条件を一枚で説明できる。

### #12　FBZZEngineのHi-Zカリング！（FBZZGraphics・前提 #11）

壁と、その奥に完全に隠れる静的メッシュを置く。Hi-Z は深度を粗い解像度までまとめた階層で、候補の投影範囲がすべて隠れているかを調べられる。`CullOccludedMeshes` と計算シェーダーを追い、遮蔽が確定した候補の間接描画引数 `instanceCount` が0になる箇所を読む。GPU が作った引数を描画に使えば、可視判定を CPU へ読み戻す往復を省ける。 [深度階層とパスの設計](design/occlusion-culling.md) · [呼び出し側](../Projects/Graphics/src/Passes/Geometry/GeometryPassHelpers.cpp#L231) · [0 または 1 を書く箇所](../Assets/Shaders/Pipeline/MeshOcclusionCull.cs.hlsl#L45)

**記事の比較**では「壁で隠れる」と「視錐台からはみ出す」を別々に描く。後者では、高密度な静的メッシュを小さな三角形のまとまり（Meshlet）に分ける経路を読む。対象全体が見える場合と十分にはみ出す場合で経路が変わる。Bindless はシェーダーが番号で資源を参照する仕組みで、この例では頂点と Meshlet データの参照を担う。 [Meshlet の選択条件](../Projects/Graphics/src/Passes/Geometry/MeshletDrawHelpers.cpp) · [GPU 駆動描画の比較条件](design/gpu-driven-geometry.md) · [Bindless の設計](design/bindless.md)

## 形状の重なりから足の接地まで

### #13　FBZZEngineの衝突判定とBVH！（FBZZPhysics・前提 #08）

まず `PhysicsSolver::BroadPhase` が返すのは「正確な形状を調べる価値があるペア」だと押さえる。各形状を包む軸平行の箱を葉に置き、親が子を包む木が BVH。入力の各箱を調べ、一つだけ変わればその葉と祖先の箱を更新（refit）する。多数の大移動や木の品質低下では再構築する。 [候補抽出の実装](../Projects/Physics/src/PhysicsSolver.cpp#L519) · [再構築の条件](design/broadphase-incremental.md)

**記事の図**は4枚の葉を持つ木にし、1個を動かしたとき書き換える箱に色を付ける。多数移動の場合は、総当たりで得た候補集合と照合するテストを添える。速度を取り上げるなら、1個移動と全移動の値を元の条件と一緒に示す。 [差分更新と総当たりのテスト](../Projects/Tests/Physics/Auto/BroadPhaseIncrementalTests.cpp#L88) · [条件別の実測](benchmarks/2026-10-22-broadphase-incremental/report.md)

### #14　FBZZEngineのGJKとEPA！（FBZZPhysics・前提 #13）

ある向きに最も突き出た点を Support 点という。A と B を逆向きに調べ、二つの点の差を集めると、形状の重なりを「差の形状が原点を含むか」という問いに変えられる。`GJK_Intersect` が交差時に残す四面体 `simplex` を `EPA_GetContactInfo` が受け取り、接触の法線と深さを求める。交差を早く判定する段階と、応答に必要な量を求める段階の受け渡しに注目する。 [GJK の入力と出力](../Projects/Physics/include/Physics/GJK.hpp) · [GJK の実装](../Projects/Physics/src/GJK.cpp) · [EPA の実装](../Projects/Physics/src/EPA.cpp)

**記事の図**は半径1の球を X=0 と X=1.5 に置いて描く。重なりは0.5、A を押し戻す法線は −X。A と B を交換したときに何が反転するかをテストと照合する。ここでは球を既知の答えがある入力として GJK と EPA に直接渡す。 [四面体の受け渡し](../Projects/Tests/Physics/Auto/GJKTests.cpp#L167) · [法線と深さのテスト](../Projects/Tests/Physics/Auto/EPATests.cpp#L46)

### #15　FBZZEngineの接触応答とCCD！（FBZZPhysics・前提 #14）

床の上の箱について、`WarmStart` が前の接触の力積を再利用し、`ResolveVelocity` が接触法線の向きに速度を補正する流れを追う。同じ相手でも接触点が大きくずれた場合は古い力積を使えない。高速な球には別の問題がある。固定刻みの始点と終点だけでは、途中で初めて触れた時刻を取りこぼす。 [接触キャッシュ](../Projects/Physics/src/ContactCache.cpp#L24) · [逐次インパルスの実装](../Projects/Physics/src/PhysicsSolver.cpp#L1280) · [接触点がずれたテスト](../Projects/Tests/Physics/Auto/ContactCacheTests.cpp#L120)

**記事の時系列**には CCD（連続衝突判定）のテストを使う。半径1の球が中心間距離10から、1ステップで10進む。初めて触れるまでの移動は8なので `toi = 0.8`。これはステップ長に対する割合で、0.1秒のステップなら0.08秒に当たる。`World` が最も早い値で移動を制限し、その後の応答を通常の接触解決に渡す行まで読む。 [接触時刻の計算](../Projects/Physics/src/CCDSolver.cpp#L13) · [数値例](../Projects/Tests/Physics/Auto/CCDTests.cpp#L22) · [移動制限への接続](../Projects/Physics/src/World.cpp#L1347)

### #16　FBZZEngineのクロスシミュレーション！（FBZZPhysics・前提 #15）

布の一辺を2頂点だけ取り出す。片方をピン留めし、もう片方を自然長より遠くに置く。`ClothSolver::SolveDistances` は現在長と自然長の差から補正量を計算し、各頂点の逆質量に応じて配る。ピン側の逆質量が0なら、そこは動かない。XPBD のコンプライアンスは拘束がどれだけ変形を許すかを表し、式には `compliance / (h * h)` として入る。`h` はサブステップの秒数。 [布の距離・曲げ拘束](../Projects/Physics/src/Cloth/ClothSolver.cpp#L361) · [XPBD ソルバー](../Projects/Physics/src/XPBDSolver.cpp) · [布の設計](design/cloth.md)

**記事の手計算**は初回の累積値 `lambda=0`、可動側の逆質量1、伸び0.2で試す。`compliance / h²` が0なら伸びの補正量は0.2、1なら0.1。布全体に戻り、伸び用と曲げ用に別の値を持つ理由を説明する。ピンの位置と伸びの上限は既存テストで確かめられる。 [ピンと伸びのテスト](../Projects/Tests/Physics/Auto/ClothSolverTests.cpp#L75)

### #17　FBZZEngineの足IKとラグドール！（FBZZPhysics の応用・前提 #08・#16）

片脚だけを追う。Engine の足 IK（足先の目標から脚の姿勢を求める処理）は地面を調べ、有効な物理足があればその形状から足首の目標を決める。Ragdoll の Active モードはアニメーション由来の姿勢へ駆動し、Passive モードはその駆動を切る。FBZZPhysics が XPBD の関節・駆動を解き、Engine の `RagdollSystem` が `WritePose` で解いた姿勢を骨へ戻す。 [足 IK の目標計算](../Projects/Engine/src/Scene/Systems/IKSystem.cpp#L405) · [モードと骨への反映](../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L568) · [物理側の駆動](../Projects/Physics/src/XPBDBodyDrive.cpp#L10) · [Ragdoll の設計](design/ragdoll-api.md)

**記事の断面図**には、足首を原点として中心が −0.08 m、高さの半分が0.04 m の足の箱を描く。水平な床に底面を載せると足首は床の0.12 m 上に来る。手動設定が0.30 m でもこの結果になるテストを読み、物理形状を使えないときはどの値へ戻るかを答える。 [足形状の数値例](../Projects/Tests/Engine/Auto/FootIKTests.cpp#L110) · [接地目標と代替値の契約](design/foot-ik-tuning.md)

## エディターの操作を追う

### #18　FBZZEngineの一括編集とUndo！（Editor・前提なし）

位置が A=(1, 2, 3)、B=(4, 5, 6) の2個を選び、Inspector の一括設定で X を9にする。`DrawTransformSetMode` は変更された軸だけを各個体に適用し、編集を終えた時点で前後の値を `PushBulkTransformUndo` に渡す。これでドラッグ中に何回値が動いても、一操作として戻せる。 [軸の判定と履歴の確定](../Projects/Editor/src/Panels/Inspector/InspectorMultiEdit.cpp#L333) · [履歴を戻す処理](../Projects/Editor/src/Util/UndoStack.cpp#L64)

**記事の比較表**に「編集前・編集後・Undo 後」の2個の座標を書く。次にメニューとパレットが呼ぶ `edit.undo` を登録側までたどる。操作 ID ごとに実行条件と本体を共有する理由を、同じ履歴が空の場合にも当てはめる。 [Undo の登録](../Projects/Editor/src/Op/BuiltinOperators.cpp#L153) · [メニューの呼出し口](../Projects/Editor/src/EditorApp_MenuBar.cpp#L370) · [パレットの呼出し口](../Projects/Editor/src/EditorApp_CommandPalette.cpp#L161) · [共通の実行条件](../Projects/Editor/src/Op/OperatorRegistry.cpp#L162)

### #19　FBZZEngineのピッキングとギズモ！（Editor・前提 #13・#18）

クリックから伸ばしたレイはワールド空間にある。メッシュ内に保存した BVH の各ノードをワールド空間の箱で包み直し、レイが通る箱の三角形だけを詳しく調べる。これで選択対象が決まる。次にギズモ（画面上の移動・回転ハンドル）を中央ピボットで動かす経路を読む。`CollectHierarchyRoots` が選択中の祖先を持たない対象を取り出し、そこに移動量を適用する。子は親の変換を継承するので、ここで子にも同じ量を足すと二重に動く。 [箱の変換とレイの照合](../Projects/Editor/src/Panels/Viewport/ViewportPicking.cpp#L84) · [ギズモの対象選別](../Projects/Editor/src/Panels/Viewport/ViewportSceneGizmos.cpp#L1535) · [選択とスナップの設計](design/editor-idle-work.md)

**記事の図**は親 X=10、子のローカル X=2、回転なし、倍率1から始める。親子を選んで X に1動かすと、子のローカルは2のまま、ワールドは12から13へ変わる。#01 の変換順を、編集操作の結果としてもう一度説明できる。

### #20　FBZZEngineの非同期サムネイル生成！（Editor・前提 #04）

Asset Browser に見えてきた画像を一枚追う。ワーカーがデコードと縮小を行い、パネル側は完了を待たずに他の表示を進める。結果が戻ったとき、要求時のパス・チケット・更新時刻を照合し、受理した画素だけを GPU テクスチャにする。遅い読み込みが、後から更新された画像を上書きするのを防ぐためだ。 [完了結果の照合と GPU 転送](../Projects/Editor/src/Panels/AssetBrowser/AssetBrowserItems.cpp#L1869) · [画像のデコード](../Projects/Editor/src/Panels/AssetBrowser/AssetBrowserThumbnail.cpp#L61) · [表示対象と処理量の設計](design/asset-browser-thumbnails.md)

**記事の時系列**に「要求 → 元画像を更新 → 古い結果が到着」を置き、どの条件で結果を捨てるかに印を付ける。画像単体の縮小で縦横比とアルファを保つテストは別に示せる。3D 材質のサムネイルでは、#04 で見た材質プレビュー用の GPU データを使う。 [縮小のテスト](../Projects/Tests/Editor/Auto/AssetBrowserThumbnailTests.cpp#L48) · [材質サムネイルの入口](../Projects/Editor/src/Panels/AssetBrowser/AssetBrowserItems.cpp#L278)

### #21　FBZZEngineのPrefabマージ！（Editor・前提 #18）

Prefab は複数の個体が参照する元アセット。子の親が「前回の元アセットでは A」「個体では B」「更新後の元アセットでは C」になった例を使う。前回の状態があれば、どちら側が何を編集したかを判定できる。`MergePrefabStructure` で競合を見つけ、プレビューから `ApplyPrefabStructureMerge` までの間に前提が変わっていないかを確かめる。 [三者マージ](../Projects/Editor/src/Util/PrefabStructureMerge.cpp#L323) · [プレビューの適用条件](../Projects/Editor/src/Util/PrefabStructureMerge.cpp#L502) · [設計](design/prefab-structural-merge.md)

**記事の比較表**は基準・個体・新版を3列にし、親が `A/A/C` の行と `A/B/C` の行を並べる。前者では新版に追従でき、後者では双方が別の親を選んでいる。さらにレビュー後に Scene か Prefab が変わったテストを読み、古いレビューの適用をどこで止めるかを示す。 [競合時の自動適用](../Projects/Tests/Editor/Auto/PrefabStructureMergeTests.cpp#L424) · [古いレビューの拒否](../Projects/Tests/Editor/Auto/PrefabStructureMergeTests.cpp#L599)

### #22　FBZZEngineのノードエディター！（Editor・前提なし）

`A → B → C` と `A → C` を持つグラフを描く。`ComputeGraphLayout` は分岐・合流のある非循環グラフで最長経路を使って列を決める。C を直接の子として B と同じ列に置くと、B から C への矢印が右に進まない。配置を説明したら、今度は B と C を選んで `ExtractSubgraph` の `idMap` に元 ID と新 ID を書く。 [自動配置](../Projects/Editor/src/GraphEditor/GraphLayoutAlgo.cpp#L20) · [最長経路を使う例](../Projects/Tests/Editor/Auto/GraphLayoutTests.cpp#L70) · [部分複製の接続規則](../Projects/Editor/src/GraphEditor/GraphSubgraphOps.cpp#L15)

**記事の図**は同じグラフの複製前後。内部の `B → C` は `B′ → C′` につながる。外から入る `A → B` は境界の扱い次第で、`KeepSource` を選ぶと `A → B′` も作る。位置だけ変える操作と、接続先の ID を作り直す操作で、元のグラフの何を使うかを説明する。

### #23　FBZZEngineのAI駆動開発！（Editor・前提 #18）

例は Viewport のコライダー表示。外部ツールから `editor_op_query` で `viewport.overlays.inspect` を照会し、`editor_op_invoke` で `viewport.overlays.configure` を呼ぶ経路を読む。標準入出力で MCP に届いた要求が、Named Pipe（プロセス間の通信路）、Editor の Command Bus、#18 の操作登録簿へ進み、結果が呼出し元へ戻る。 [MCP の構成](../Projects/EditorMcp/README.md) · [パイプ通信](../Projects/EditorMcp/src/busClient.ts) · [Editor 側の受付](../Projects/Editor/src/Ai/Bus/EditorHandlers.cpp#L805) · [照会と変更の例](design/EditorDirectApi.md)

**記事の往復図**では、`read` で現状を読む、`dry-run` で変更の引数と実行条件を調べる、`write` で適用して照会し直す、のそれぞれで何が分かるかを書く。時間のかかる操作の例として Playtest も載せる。`scenario_run` の返答を「開始受付」、`scenario_status` の `running / passed / failed` を「進捗と結果」の位置に置く。 [操作呼び出しの実装](../Projects/EditorMcp/src/Tools/EditorTools.ts) · [Playtest の開始と結果照会](../Projects/EditorMcp/src/Tools/PlaytestTools.ts)

## 書き終えたときの答え合わせ

自分の図だけを見て、次の四つを誰かに説明してみる。説明が途切れた矢印が、もう一度コードを開く場所になる。

- **一つの球が画面に出るまで。** ローカルの印をワールド座標に変え（#01）、描画側に渡る値とハンドルを分け（#02）、CPU と GPU の完了を図に置く（#03）。視点だけを変えた次のフレームで、何を再利用できるかを説明し（#04）、描画入力・描画経路・資源回収の判断をどこが引き受けるかを整理する（#05）。
- **1フレームの描画を減らすとき。** カメラの外にある球（#08）、最終画像に届かないパス（#09）、壁に隠れたメッシュ（#12）を一つずつ挙げる。それぞれ、何を根拠にどの処理を省き、効果をどの区間で測るかを言えるか（#07）。
- **一つの接触が動きに変わるまで。** 箱の重なりから候補ペアを作り（#13）、正確な形状から法線と深さを求め（#14）、速度や位置をどう直すかを示す（#15・#16）。そのうち何が足の目標で、何が解いた結果なのかを区別できるか（#17）。
- **一つの編集を確かめるまで。** 親子を選んで移動し（#19）、Undo が何を戻すかを示す（#18）。同じ Editor を外部から呼ぶとき、実行条件・変更後の状態・非同期処理の完了をそれぞれどの返答で確かめるかを話せるか（#23）。
