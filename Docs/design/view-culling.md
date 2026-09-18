# ビューごとのカリングを 1 回にまとめる

`Docs/design/math-simd.md` 段 4。今は描画パスが物体ループの中で 1 個ずつ境界を計算して錐台と比べており、同じビューで同じ物体を 2〜3 回判定している。これをビューごとに 1 回の «集める → 一括で判定する → 各パスが結果を引く» に直す。

## 1. 今の形

| パス | 判定の入口 | ループ |
|------|-----------|--------|
| Forward (静的 / スキン) | `IsMeshVisible` / `IsSkinnedVisible` (`ForwardPasses.cpp:119, :173`) | `ctx.scene.GameObjects()` を全走査 |
| GBuffer (Deferred / Forward の前段) | `IsMeshVisible` (`DeferredPasses.cpp:138`) | 同上 |
| Deferred の半透明・Forward 専用・スキン | `DeferredPasses.cpp:412, :520, :578` | 同上 |
| Velocity | `VelocityPass.cpp:115, :163` | 同上 |
| 遮蔽カリング | 判定の後に `ComputeWorldBounds` をもう一度 (`ForwardPasses.cpp:281`, `DeferredPasses.cpp:163`) | ソート済みの列 |
| 影 | `CollectStaticMeshShadowCasters` / `CollectSkinnedMeshShadowCasters` がカスケードごとに `IntersectsSphere` (`ShadowPass.cpp:85-133`) | フレームに 1 回の収集 |

- 境界 (`ComputeWorldBounds`) は判定のたびに作り直している。`GetWorldMatrix` と sqrt 3 回。スキンは親をたどって `GetComponent<AnimatorComponent>` を引く
- 判定の回数: Deferred で約 2 回 (GBuffer か半透明 + Velocity)、Forward + 前段 GBuffer (AO / SSR / 接触影が有効なとき) で 3 回
- 境界の余白はどのパスも `ctx.cullingBoundsPadding` で同じ。Fiber だけ毛の長さぶん別の余白を足す
- 統計 (`statsFrustumCulled` ほか) は Forward + 前段 GBuffer で**二重に数えている** (Velocity は退避・復元しているが前段 GBuffer はしていない)。Analysis パネルと AI バスの «描いた数» がずれる

## 2. 先に測る (段 4-0)

平面判定そのものは段 3 で 1 個 1.6 ns になった。重いのは境界の再計算と `GetComponent` のほうだと見ているが、測っていない。**作り直す前に、今の判定にかかる時間を測る。**

- `IsMeshVisible` / `IsSkinnedVisible` の時間と呼び出し回数を `RenderPassContext` に足し込み、`RenderStats` を経て AI バスの `profiler.snapshot` に出した。計時の費用 (`steady_clock` 2 回) は起動時に 1 度測って併記し、回数倍を差し引いて読んだ
- 場面: GreenWare の Stage_01〜03 を Play し、入力なしで 120 フレーム進めてから 30 フレームおきに 10 回 `profiler.snapshot` を取るシナリオを `--batch` で回した
- scenario の `bus` で `query` を投げたときの応答は報告 (`report.json` の `steps[].result`) に残るようにした (PlaytestRunner。これは残す)
- 計時のコードとシナリオは見送りと決めた後に外した。計時は 1 回 25 ns で判定そのものと同じくらいかかるため。測り直すときは上の手順で入れ直す
- **進む条件**: 合計がビューあたり 0.2 ms (60fps の 1 フレームの約 1%) 以上なら 4-2 へ進む。下回れば段 4 はここで止める

### 結果 (2026-09-19) — 見送り

`FBZZEditor.exe --batch` で 3 ステージを回した 10 回の平均 (Development / i7-14700F + RTX 4070)。正味 = 計時込みの値 − 回数 × 計時の費用 (25 ns)。

| ステージ | 判定の回数 / フレーム | 正味 / ビュー | 1 回あたり | 計時込みの最大 |
|---|---:|---:|---:|---:|
| Stage_01 | 152 | 0.0062 ms | 41 ns | 0.012 ms |
| Stage_02 | 647 | 0.0212 ms | 33 ns | 0.041 ms |
| Stage_03 | 22 | 0.0018 ms | 83 ns | 0.004 ms |

- 最も重い Stage_02 でも基準の約 1/10。**4-2 以降は行わない**。1 物体を 2〜3 回判定していても、1 回が 30〜80 ns なので合計は描画 CPU 時間 (Stage_02 で約 18 ms) の 0.1% 程度にしかならない
- Stage_03 は描画対象 1051 のうち `IsMeshVisible` / `IsSkinnedVisible` を通るのが 22 回だけ。残りは地形・Fiber・パーティクルなど別の入口で判定している
- 統計の二重計上 (§1) は 4-2 とは切り離して直した (`c9ab285a`)。前段 GBuffer と地形の前段が `CullStatsRollback` で加算を打ち消す。Stage_02 を Forward+ で回し、候補数・錐台・可視の数が Deferred+ と一致することを確かめた
- 物体数が 1 桁増えるステージを作るときは、§2 の手順で測り直す

## 3. 決めたこと (段 4-2)

### 3.1 どこで判定するか

`RenderSystem()` が `pipeline.Execute(passCtx, …)` を呼ぶ直前 (`RenderSystem.cpp` の錐台を `passCtx` へ入れた後) に、ビューごとのカリング `CullView(passCtx)` を 1 回呼ぶ。

- RenderGraph のパスにはしない。GPU リソースを読み書きしないパスは依存が無いとして捨てられうる (`render-graph.md`)。カリングはパスの前提を作る CPU の準備で、錐台や距離の設定と同じ層に置く
- `FBZZ_PROFILE_SCOPE("RenderSystem::CullView")` で囲み、Profiler に出す
- LOD (`lodVisible`) と Animator の骨はこの時点で更新済み (どちらも描画より前の Phase)

### 3.2 何を持つか

```
ViewCullResult  (ビューごと、EntityID.index で引く)
  sphere : Vector4   xyz = 中心、w = 半径 (余白込み)。w <= 0 は境界なし
  state  : uint8     Visible / NoBounds / Distance / SmallObject / Frustum / NotGathered
  generation         EntityID.generation。一致しなければ «集めていない» 扱い
```

- 集める: `GameObjects()` を 1 回走査し、`MeshRenderer` か `SkinnedMeshRenderer` を持つものだけ境界を作って詰める
- 判定: 錐台は `Frustum::IntersectsSpheres` で一括。距離と極小は今の `TestBoundsVisible` と同じ式・同じ優先順 (距離 → 極小 → 錐台) で理由を 1 つに決める
- 配列は `ViewRenderTargets` に置き、フレームをまたいで容量を使い回す (ビューをまたぐ状態を static に置かない。`RenderPassHandles` は毎フレーム作り直すので不可)

### 3.3 パスからの引き方

- `IsMeshVisible` / `IsSkinnedVisible` の中身を «結果を引く» に替える。呼び出し側のループは変えない
- 結果が無いとき (`NotGathered`: 集めた後に生まれた物体・世代違い・結果を持たない派生コンテキスト) は今までどおりその場で計算する。挙動は変えず、落ちない
- 遮蔽カリング前の `ComputeWorldBounds` もキャッシュした境界を使う
- 統計は `CullView` が 1 回だけ数え、各パスの加算はやめる。**二重計上が直るので、Forward + 前段 GBuffer の構成では表示される «カリング数» が約半分に変わる** (正しい値へ)

### 3.4 影 (段 4-3)

- キャスターの収集でもキャッシュした境界を使う (同じ余白)
- カスケード・Spot/Point の各錐台について `IntersectsSpheres` を 1 回ずつ呼び、ビットを立てて `cascadeMask` / `punctualMask` を作る。テクセル大きさの判定は今の関数のまま
- スキンのサブメッシュごとの再判定 (`ShadowPass.cpp:276-279`) は対象外 (サブメッシュ単位の境界はキャッシュしない)

## 4. 正しさの確かめ方

- **判定の結果は 1 物体も変わらない**。境界は同じ関数・同じ余白で作り、錐台の一括判定は単体版と一致する (`ScalarParityTests.BatchedSphereCullingMatchesOneByOne`)。したがって Playtest の基準画像は画素単位で一致しなければならない
- Debug ビルドでは引いた結果とその場で計算した結果を突き合わせ、ずれたら物体名つきで報告する
- 統計の数だけは変わる (§3.3)。二重計上は 4-0 の後に単独で直した (§2 結果)

## 5. 段取り

| 段 | 内容 | 確かめ方 |
|----|------|---------|
| 4-0 | 判定にかかる時間を測る (挙動は変えない) | §2 の場面で数字を取り、進むか決める |
| 4-2 | `CullView` とキャッシュ。カメラ用のパスと遮蔽カリングが結果を引く。統計の二重計上も直る | 基準画像が一致・§2 の手順で 4-0 と比べる |
| 4-3 | 影の収集がキャッシュした境界と一括判定を使う | 同上 (影の基準画像を含む) |

## 6. やらないこと (今は)

- Fiber (余白が違う)・パーティクル (エミッターの境界)・地形と水 (チャンクの AABB): 入口も境界の出どころも別
- 境界をビューをまたいで共有する (Scene View と Game View で同じ物体の境界は同じ): 効くが、フレームの印で無効化する仕組みが要る。4-2 の数字を見てから
- 集める処理の並列化・GPU カリング
