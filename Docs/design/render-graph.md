# RenderGraph — 何を担い、何を担わないか

`fbzz::renderer::RenderGraph` と `fbzz::scene::RenderPipeline` の役割分担と、
**意図的に作っていない機能とその理由**をまとめる。

実装は `Engine/Renderer/RenderGraph.hpp`（依存解決器・GPU を触らない）と
`Engine/Scene/Systems/RenderPasses/RenderPipeline.{hpp,cpp}`（登録・実行）。

---

## 担っていること

### 実行順を依存から導く

登録順が持つ役目は 2 つだけ。

1. 同じリソースへ複数のパスが書くときの **世代の順序**（Sky は Geometry の上に描く）
2. 依存が何も言っていないときの **タイブレーク**（`SchedulePolicy::RegistrationOrder`）

どちらも意図であって偶然ではないので、そこだけは登録順に従う。それ以外の順序は
申告された依存が決めるため、生産者を消費者より後ろに登録しても正しく並ぶ。

RAW / WAW / WAR はリソースの「世代」の上で張り分ける。生存判定（デッドパスカリング）は
**RAW だけ**を辿る — WAW / WAR は順序の制約であって必要性ではないため。

### 申告と実体を同じ名前で結ぶ

`PassBuilder`（Setup）で申告した名前だけが `PassResources`（Execute）から引ける。
名前 → 実ハンドルの対応は `RenderPipeline::DeclareTarget` / `DeclareTexture` が
申告と一緒に受け取り、`Execute()` の冒頭で登録簿へ流す。
**申告と実体を別々に書く経路は残っていない。**

### Plan のキャッシュ

構成（リソース記述・出力・パス名・accesses・allowCulling・スケジュール方針）の
FNV-1a 指紋が前フレームと一致すれば、依存解決・カリング・寿命解析をまるごと省略して
前フレームの結果を注入する。指紋に混ぜ忘れた項目は「変わっても同じ鍵」になり、
古い実行順を使い回す故障になる。`RenderPipelineTests` が項目ごとに固定している。

---

## 意図的に作っていないこと

### グラフ駆動のリソース状態遷移（バリア）

**担当は各バックエンド。** DX12 は `DX12StateTracker` がリソースごとに状態を追跡し、
束縛サイトで `QueueTransition` して 1 回の `ResourceBarrier` にまとめて発行する
（`DX12Renderer.cpp`）。DX11 は暗黙遷移なので何も要らない。

グラフ駆動のバリアはこの上に乗る **最適化**（冗長な遷移の除去、split barrier、
パス跨ぎのバッチ）であって、欠けている土台ではない。現状で正しく動いているものを
二重化する利得が無いので作っていない。

やるとすれば `ExecutionReport` に遷移列を足し、DX12 側でそれを消費して
`DX12StateTracker` の束縛時追跡を置き換える形になる。**着手の条件は
「冗長なバリアが実測でフレーム時間に出ていること」**。

### トランジェントリソースのエイリアスプール

**一度作って、実測の上で消した。** alias グループごとに物理リソースを確保して
貸し回すプールがあったが、貸出先が 1 つも無いまま、グループの数だけ実体を確保していた。
実測（`Artifacts/RenderGraph` の構成テキスト）ではトランジェント 7 個が 7 グループに
分かれて 1 枚も共有できておらず、それでも 7 枚ぶんの VRAM（1 ビュー約 34MB）を握っていた。
**節約ゼロで消費だけがある状態**だったので、使う当てができるまで確保しない。

寿命とエイリアスグループの解析そのものは `RenderGraph::AnalyzeLifetimes` に残っている
（構成テキストの `alias=` がそれ）。パスが増えて寿命が分かれたら、
**まず構成テキストで「何枚浮くか」を測ってから**作り直すこと。

`SchedulePolicy::MinimizeLifetimes` は同時生存数を減らす方向へ実行順を寄せるので、
プールを作り直すときの前提条件になる。

### `LambdaPass` の全廃

**到達不可能なので目指さない。** `UserRenderPassDesc::execute` は
`std::function<void(RenderPassContext&)>` で、スクリプトが登録するパスは永久にラムダ経路。
ポストプロセス連鎖（Composite / TAA / FXAA / Upscale / SelectionOutline）も
`ppCurrent` / `chainOutRes` という **実行時に決まる ping-pong 名** を申告するため、
クラスの `Setup()` には書けない。

`LambdaPass` は移行中の足場ではなく **恒久的な第 2 の登録形式**。

ただしエンジン自身のパスは型付き（`IRenderPass`）にする。理由は美意識ではない:

> ラムダで登録すると申告が `RenderSystem.cpp` に残り、本体のあるファイルから
> 200 行以上離れる。本体が新しいテクスチャを読み始めても申告を直す場所が視界に入らない。

実際に `TerrainRenderPass` と `DeferredLighting` で「読んでいるのに申告していない」が
起き、どちらも型付きへ移す作業の中で見つかった（`TerrainRenderPass::Setup` のコメント参照）。
**申告を本体の隣へ置けるものは置く**、が方針。

---

## 変更したときの確かめ方

`RenderGraph::DescribeLastPlan()` は直前の Plan を「差分の取れるテキスト」にする
（実行順・カリング・エイリアス割り当て。計測値は入れない — 毎フレーム変わると差分が
意味を失うため）。`RenderPipeline::LastPlanDescription()` から取れ、
構成が変わったフレームだけ更新される。

パスを移設・整理したときは **前後で 1 回ずつ保存して差分を見る**。
同一なら挙動不変が証明でき、違っていればそれが申告漏れ。
絵を見ても「実行順が変わったこと」は分からないので、これが唯一の手掛かりになる。

エディターからは `Render Pass Viewer` の 3 タブで見る。

| タブ | 見えるもの |
|---|---|
| Passes | 実行順・CPU / GPU 時間・選択パス直後の画像・パス単位の上書き |
| Graph | 依存の深さを列にしたノード図。辺は申告から導出した RAW 依存 |
| Resources | フレーム末尾の全リソース。申告と実体の食い違いをバッジで表示 |
