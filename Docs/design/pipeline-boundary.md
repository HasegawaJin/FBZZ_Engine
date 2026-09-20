# Deferred と Forward の境界

- 状態: 材質の経路規則とその導入背景。2026-09-20 の作業ツリーには `GeometryRoute` とスキンド GBuffer の実装があるが、本書の「今の形」は導入前の状況を記録している。検証済みという意味ではない。
- 続く構造設計: [Graphics ライブラリと Forward / Deferred の境界](graphics-library.md)。本書は材質ごとの経路、新設計はライブラリ・パス構成・入力と寿命の境界を扱う。

「Deferred を選んだら Deferred で動く」を、規則 1 本と分岐 1 か所で言い切れる形にする。

## 1. 今の形 — Deferred の中で Forward が動く 4 つ

| 対象 | 条件の場所 | 理由 | 本質的か |
|------|-----------|------|---------|
| 半透明 (static / skinned) | `GetBlendMode() != OPAQUE_BLEND` | GBuffer は 1 画素 1 面しか持てない | **本質的** |
| `IsForwardOnly` な不透明 static | `DeferredPasses.cpp:149` / `:594` | GBuffer 2 枚に拡張ローブと接線基底が入らない | **能力の境界** |
| 毛 (Fiber) | `FiberRenderPass` | 殻を重ねる独自描画 | **本質的** |
| **スキンドの不透明 — 全部** | `DeferredPasses.cpp:423` | **無い** | **歴史的** |

### 1.1 判定が 3 か所に散っている

同じ「GBuffer に入るか」を、GBuffer 収集は `if (IsForwardOnly) continue`、Forward 不透明は
`if (!IsForwardOnly) continue`、スキンドは無条件 Forward、と別々に書いている。
**互いに補集合であることを保証する仕組みが無い。** 片方に条件を足すと、物が二重に描かれるか黙って消える。

### 1.2 判定の根拠が「パラメーターの有無」になっている

`IsForwardOnly` (`GeometryPassHelpers.cpp:354`) が見ているのは clearcoat / sheen / anisotropy /
clothSheenColor の値と、`shaderPath` が空かどうか。ここから 2 つの取りこぼしが出る。

- **`shader = ""` が Forward へ落ちる** — 空欄は Fallback (標準 PBR 相当) で描かれるので GBuffer に
  入れて問題ない。GreenWare には 52 個ある
- **シェーディングモデルが違う材質が GBuffer へ入る** — `Unlit` / `Toon` / `RimLight` / `Dissolve` は
  拡張ローブを持たないので判定を素通りし、不透明なら GBuffer へ入って**標準 PBR として描かれる**。
  Unlit を使う .mat は GreenWare に 4 個ある

**GBuffer パスは材質のシェーダーを捨てて `GBuffer.hlsl` で描く。** だから安全な規則は 1 つしかない。

> GBuffer に入れてよいのは、**エンジンが「GBuffer 相当」だと知っているシェーダー**だけ。
> 知らないものは Forward。

パラメーターでなくシェーダーで決めるので、ユーザーが書いたカスタムシェーダーは自動的に Forward
(＝常に正しいが遅いほう) へ倒れる。**安全な側が既定**になる。

## 2. 規則を 1 本にする

```cpp
enum class GeometryRoute : uint8_t {
    GBuffer,            ///< GBuffer へ書き、DeferredLighting が後で解く
    ForwardOpaque,      ///< 不透明だが Forward で完全評価する
    ForwardTransparent, ///< 半透明。深度順に後から描く
};
```

判定の順序 (上から順に当てはめ、最初に当たったものを採る):

| # | 条件 | 経路 | 理由 |
|---|------|-----|------|
| 1 | 半透明 | `ForwardTransparent` | GBuffer が表せない |
| 2 | GBuffer 経路でない (Forward / Forward+) | `ForwardOpaque` | そもそも GBuffer が無い |
| 3 | シェーダーが GBuffer 相当でない | `ForwardOpaque` | §1.2 |
| 4 | 拡張ローブを持つ | `ForwardOpaque` | 2 枚に収まらない |
| 5 | それ以外 | `GBuffer` | |

**政策 (どう決めるか) と事実集め (材質を引く) を分ける。** 政策は純粋関数にしてテストで固定し、
アセットを引く部分は呼び出し側に置く。

```cpp
struct GeometryRouteInput {
    renderer::BlendMode blend;
    bool gbufferPipeline;       ///< Deferred / Deferred+ か
    bool gbufferEquivalentShader;
    bool advancedLobe;
};
[[nodiscard]] GeometryRoute ResolveGeometryRoute(const GeometryRouteInput& input);
```

### 2.1 「GBuffer 相当」のシェーダー

エンジンが自分で持っているシェーダーのうち、`GBuffer.hlsl` に置き換えても絵が変わらないもの。

| 種別 | シェーダー |
|------|-----------|
| Surface | `PBR.hlsl` / `Lit.hlsl` / `Fallback.hlsl` |
| Skinned | `SkinnedPBR.hlsl` / `SkinnedLit.hlsl` / `FallbackSkinned.hlsl` |
| 空欄 | Fallback として扱う (＝ GBuffer 相当) |

判定はファイル名で行う。シェーダーは `Assets/` と `<Project>/Assets/` の 2 本立てで、参照は
`guid:` にもパスにもなるため、絶対パスで比べると同じシェーダーが別物に見える。

`Toon` / `RimLight` / `Unlit` / `Dissolve` / `Anisotropic` / `Subsurface` / `Cloth` と、
プロジェクト側のカスタムシェーダーはすべて **GBuffer 相当ではない** = Forward。

## 3. スキンドを GBuffer へ入れる

スキンドが Forward である技術的理由は無い。**手はもう揃っている。**

`SkinningComputePass` はボーン変形の結果を**静的メッシュと同じ頂点レイアウト**で書き出していて、
`ShadowPass` はそれを使ってスキンドを「ただの静的メッシュ」として影に描いている
(`ShadowPass.cpp:293-300`)。GBuffer も同じ手が使える。

| 状態 | 描き方 |
|------|-------|
| コンピュートスキニング済み | 既存の `GBuffer.hlsl` をそのまま使う (頂点はもう変形済み) |
| 未済み (フォールバック) | `GBufferSkinned.hlsl` — b7 のパレットで VS が変形し、PS は共有 |

`IsForwardOnly` の中にある `a->meshType != MeshType::Skinned &&` という除外を外す。
これは「どうせスキンドは GBuffer に行かない」という前提が判定側へ染み出したもので、
スキンドが GBuffer へ入るなら拡張ローブの判定はスキンドにも要る。

**結果として Deferred に残る Forward は 3 つだけ**になり、どれも Deferred の定義上そうなるものになる。

- 半透明
- GBuffer に収まらない材質 (§2.1 の外側)
- 毛

## 4. Forward 側の名前

Forward が走らせている `GBufferPass(GBufferPassMode::ForwardPrepass)` は、**ライティングには使わない**。
SSAO / GTAO / SSR / 接触影へ深度と法線を渡すためだけのプリパスで、DeferredLighting は動かない。

名前が `GBufferPass` なので「Forward にも GBuffer がある」と読めてしまう。
**`DepthNormalPrepass` へ改名**し、Forward に GBuffer は無いことを構造で示す。

## 5. 絵が変わるところ

| 変わるもの | 向き |
|-----------|------|
| `shader = ""` の不透明材質 (GreenWare に 52) | Forward → GBuffer。SSAO / SSR / 接触影が正しく掛かる |
| `Unlit` / `Toon` / `RimLight` の不透明材質 | GBuffer → Forward。**PBR として陰影が付いていたのが止まる** |
| スキンドの不透明 (標準 PBR のもの) | Forward → GBuffer。SSAO / SSR / 接触影の掛かり方が変わる |
| スキンドのカスタム材質 | Forward のまま (変わらない) |

基準画像は撮り直す。**どれも「今が偶然そう見えていた」側が直る変更**なので、差分は
「そうなるはず」の向きに出ているかを目で見て確かめる。

## 6. やらないこと

- **Forward 経路の削除**。半透明と表現の逃げ道として必ず要る
- **GBuffer の枚数を増やす**。拡張ローブを入れるなら 3 枚目が要るが、それは別の話
- **`render_path` での上書き**。`.mat` の `render_path` は Particle / Trail / UI / Decal / PostProcess の
  区別で、ライティング経路の指定ではない。意味を二重にしない

## @see

- [graphics-library.md](graphics-library.md) — Graphics 分離、共通 Forward パス、段境界と移行・検証の設計 (Draft)
- `Docs/design/render-graph.md` — パスの依存申告
- `Docs/design/gpu-instancing.md` — GBuffer / Shadow の束ね。経路が変わると束ねの単位も変わる
