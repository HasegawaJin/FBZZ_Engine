# 同じメッシュを 1 回の描画へ束ねる (自動インスタンシング)

同じメッシュ・同じマテリアルの物体を、シーンの書き方を変えずに 1 回の `DrawIndexedInstanced` へ束ねる。**既存のシーンがそのまま軽くなる**ことを条件にするので、`InstancedMeshRenderer` のような新しいコンポーネントは足さない。

## 1. 今の形

`DrawCall` はインスタンシングの口を最初から持っている (`DrawCall.hpp:46-50` の `instanceCount` / `instanceBuffer`)。DX12 側も `DrawIndexedInstanced` を発行し (`DX12Renderer.cpp:397-403`)、instanceBuffer を VS の t0 (`VS_SB_INSTANCE_SLOT`) へ束縛する経路まで通っている。

**使っているのは毛 (`FiberRenderPass.cpp:597` の殻) と GPU パーティクル (`ParticlePass.cpp:1776`) だけ**で、`MeshRenderer` は 1 個 = 1 ドローのまま。同じ柵を 200 本置けば 200 ドロー、影が 4 カスケードなら合計 1000 ドローになる。

| パス | 物体ループ | 使うシェーダー | 1 物体あたりの発行数 |
|------|-----------|--------------|-------------------|
| GBuffer (Deferred 不透明) | `DeferredPasses.cpp:192` | `handles.gbufferShader` 1 本 | 1 |
| Shadow | `EmitShadowCasters` (`ShadowPass.cpp:317`) | `handles.shadowShader` 1 本 | カスケード数 + Spot/Point の面数 |
| Velocity | `VelocityPass.cpp` | `Motion/Velocity.hlsl` 1 本 | 1 (TAA / Motion Blur 有効時) |
| ObjectMask | `ObjectMaskPass.cpp` | `Pipeline/Mask/ObjectMask.hlsl` 1 本 | 要求のある物体だけ |
| Forward 不透明 | `ForwardPasses.cpp` | **マテリアルごとに違う** (`material->shader`) | 1 |

**束ねられるのは「エンジンが 1 本のシェーダーで描くパス」だけ**である点が設計の分かれ目になる。Forward の不透明はユーザーが書いた `.mat` のシェーダーをそのまま使うので、インスタンス化するには全マテリアルシェーダーに変種が要る。ここは対象にしない。

## 2. 何を per-instance にするか

今は物体ごとに `PerObjectCB` (b1) を書き換えて Submit している (`DeferredPasses.cpp:255-259`)。中身は 3 つ。

| 枠 | 用途 | per-instance にするか |
|----|------|---------------------|
| `world` | ワールド行列 | **する** |
| `worldInvTranspose` | 法線用の逆転置 | **する** |
| `objectParams.x` | LOD ディザのしきい値 | **しない** — §2.1 |

### 2.1 LOD ディザは束ねない

`objectParams.x` は PS (`ApplyLodDither`) が読む。per-instance にすると VS→PS へ 1 枠増やすことになり、`PSInput` (`Common/Structs.hlsli`) は全シェーダーの共有構造体なので波及が大きい。

**LOD 遷移中 (`lodDither != 0`) の物体は束ねる対象から外す**。遷移しているのは一度に数個で、束ねられなくても失うものが無い。束ねた描画は b1 に objectParams = 0 のまま束縛するので、PS は従来どおり「遷移していない」として読む。

これにより per-instance データは行列 2 本だけになる。

```cpp
/// LAYOUT: Assets/Shaders/Common/ObjectInstance.hlsli の ObjectInstance と一致させること。
struct PerInstanceData {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
};
```

## 3. 束ねてよい条件

同じ `DrawCall` になる物体だけを束ねる。判定は **b1 と instanceBuffer / instanceCount を除いた全フィールドの一致**とする (`IsSameInstanceBatch`)。

鍵を別に定義して「メッシュとマテリアルが同じなら束ねる」と書かないのは、テクスチャ 32 枠・定数バッファ 14 枠のどれか 1 つでも違えば絵が変わるため。鍵を手書きすると、後から枠を増やしたときに**束ね条件だけが古くなって別のテクスチャで描かれる**という、絵を見ても原因に辿りつけない壊れ方をする。

比較は「直前に積んだ 1 件」とだけ行う (O(n))。総当たりのハッシュにしないのは、各パスが既にマテリアル順へ並べ替えてあり (`DeferredPasses.cpp:183`)、同じ束は連続して現れるため。

## 4. シェーダーの変種

`Constants.hlsli:29` の `FBZZ_OBJECT_CONSTANTS` を使う。Velocity パスが b1 の 2 枠目を `prevWorld` として使うために既に用意されている仕組みで、同じ規約に乗る。

`Common/ObjectInstance.hlsli` を足し、変種側のファイルは 2 行だけにする。

```hlsl
// Pipeline/Deferred/GBufferInstanced.hlsl
#define FBZZ_INSTANCED 1
#include "GBuffer.hlsl"
```

本体側は VS の入口だけを分ける。本体 (`GBufferVS`) は共有するので、片方だけ直し忘れることがない。

```hlsl
#ifdef FBZZ_INSTANCED
PSInput VSMain(VSInput v, uint iid : SV_InstanceID)
{ return GBufferVS(v, gObjectInstances[iid].world, gObjectInstances[iid].worldInvTranspose); }
#else
PSInput VSMain(VSInput v) { return GBufferVS(v, world, worldInvTranspose); }
#endif
```

- instanceBuffer は `FBZZ_VS_SBUFFER(..., VS_SB_INSTANCE_SLOT)` で宣言する。bindless 移行中でも他のバッファと同じ経路に乗る
- シェーダーは `Assets/Shaders/` と `GreenWare/Assets/Shaders/` の 2 本立てなので、**変種も両方へ置く** (AGENTS.md «アセット»)

## 5. 対象にするパス

| パス | 対象 | 理由 |
|------|-----|------|
| Shadow | **する** | 1 物体がカスケード数 + Spot/Point 面数ぶん出る。削減量が最大 |
| GBuffer (Deferred 不透明) | **する** | エンジンのシェーダー 1 本で全不透明を描く |
| Velocity | **する** | per-instance の 2 枠目の意味だけが違う (`worldInvTranspose` → `prevWorld`)。§5.1 |
| ObjectMask | **する** | 同じ仕組み。申告 1 件ぶんが束ねの単位。§5.2 |
| Forward 不透明 | **しない** | マテリアルのシェーダーをそのまま使うため変種を用意できない (§1) |
| スキンド | **しない** | 頂点バッファが物体ごとに違う (スキニング結果) ので、そもそも同じ描画にならない |

### 5.1 Velocity — 2 枠目の意味だけが違う

`Velocity.hlsl` は `FBZZ_OBJECT_CONSTANTS` で b1 を `{ world, prevWorld }` として宣言し直している。
per-instance 側も同じ 128 バイトのままで、読む名前だけが変わる
(`ObjectInstance.hlsli` の `ObjectInstanceMotion`)。

**束ねる側は 2 本の行列を解釈しない。** 意味付けはパスの C++ と HLSL の対で閉じるので、
`InstanceBatcher` に Velocity 用の分岐は要らない。

速度を描くのは「前フレームから動いた物体」だけなので、束ねられるのは**同じメッシュが揃って
動いているとき** (回る歯車・同じ動きの群れ) に限られる。静止物はそもそもこのパスに出てこない。

### 5.2 ObjectMask — 束ねの単位は申告 1 件

b2 (`objectMaskCB`) は申告ごとに書き換わるハンドル共有の CB なので、申告を跨いで束ねると
**先に積んだシルエットまで後の色で描かれる**。束ねるスコープを申告 1 件に閉じる。

木を再帰で降りる経路なので、スキンド用の `Flush()` は「スキンドを実際に描くと決まってから」
呼ぶこと。判定より前に置くと、スキンドを持たない GameObject でも束が切れて 1 つも束ねられない。

同じ経路はスキンド描画で b1 を自分で書くため、`InvalidateObjectConstants()` で束ね側の
「載せ直しの省略」を明示的に捨てる。

## 6. 測り方

`Docs/design/view-culling.md` と同じく、**入れる前に今の数字を残す**。

- `RenderPassContext` の `statsDrawCalls` / `statsShadowDrawCalls` が「実際に発行した数」。三角形・頂点の統計は `DrawCallTriangleCount` が既に `instanceCount` を掛けているので、束ねても総量は変わらず**ドロー数だけが落ちる**
- 束ねた回数と、束ねたことで減ったドロー数を `statsInstancedBatches` / `statsInstancedDrawsSaved` として並べる
- `RenderSettings::gpuInstancing` で切れるようにする。同じシーンで on / off を切り替えて比べられないと、減った数が何によるものか言えない
- 測る場面: 同じメッシュを並べた検証シーンと、GreenWare の Stage_01〜03

## 7. やらないこと

- **描画順の入れ替え**。GBuffer は既にマテリアル順、影は深度順に並んでいる。束ねるために並べ替えると、前者はそのまま、後者は early-Z の効きが変わる。今回は**既存の並びの中で連続している同一描画だけ**を束ねる。並べ替えの損得は測ってから別に決める
- **Forward / スキンドへの拡張** (§5)
- **GPU 側でのカリング・間接描画**。CPU で束ねた結果を `DrawIndexedInstanced` へ渡すところまで。`ExecuteIndirect` と GPU カリングはこの上に載る別の段

## @see

- `Docs/design/view-culling.md` — 測ってから作るときの手順と «進む条件» の書き方
- `Docs/design/bindless.md` — instanceBuffer も bindless 添字の vertex[0] を通る (`DX12Renderer.cpp:306`)
- [D3D12 Instancing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/drawinstanced) — `DrawIndexedInstanced` と `SV_InstanceID` の契約
