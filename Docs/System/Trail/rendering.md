# Trail Renderer 設計 — レンダリング詳細

## TrailComponent データ設計

```cpp
namespace fbzz::scene {

struct TrailPoint {
    math::Vector3 position;
    float         timestamp;  // Application::GetTime() 絶対時刻
};

enum class TrailAlignment : uint8_t {
    CameraFacing,  // cross(segDir, toCamera)
    WorldUp,       // cross(segDir, (0,1,0))
};

struct TrailComponent {
    // --- パラメータ ---
    float          duration         = 1.0f;    // 制御点の寿命（秒）
    int            maxPoints        = 64;      // リングバッファ容量（初期化後変更不可）
    float          sampleInterval   = 1.0f / 30.0f; // サンプリング間隔（秒）
    float          minVertexDist    = 0.02f;   // 最小移動距離（停止中は追加しない）
    float          widthStart       = 0.2f;    // 最新点（先端）の幅
    float          widthEnd         = 0.02f;   // 最古点（末尾）の幅
    math::Vector4  colorStart       = { 1,1,1,1 };
    math::Vector4  colorEnd         = { 1,1,1,0 };
    TrailAlignment alignment        = TrailAlignment::CameraFacing;
    int            smoothSubdivisions = 0;     // Catmull-Rom サブディビジョン回数（0=無効）
    std::string    texturePath      = "";      // 空でデフォルトホワイト
    float          uvScrollSpeed    = 0.0f;    // U 方向スクロール速度（単位/秒）。正=末尾→先端、負=先端→末尾
    float          uvTiling         = 1.0f;    // U 方向タイリング回数（炎は 2〜4 程度）

    // --- リングバッファ（System が読み書き、Init 時に reserve） ---
    // WHY: std::deque は push/pop のたびにヒープ断片化しキャッシュ局所性が低い。
    //      固定長配列 + head/tail インデックスなら初期化後は一切アロケーションしない。
    std::vector<TrailPoint> pointBuffer;  // reserve(maxPoints) 済み、ring として使う
    int   ringHead       = 0;  // 最古点のインデックス
    int   ringTail       = 0;  // 次書き込みインデックス
    int   ringCount      = 0;
    float lastSampleTime = -1.0f;

    // --- GPU リソースハンドル（System が管理） ---
    renderer::ResourceHandle<renderer::BufferTag>        vertexBuffer;  // DYNAMIC
    renderer::ResourceHandle<renderer::ShaderTag>        shader;
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState;
    renderer::ResourceHandle<renderer::TextureTag>       texture;
    renderer::ResourceHandle<renderer::ConstantBufferTag> trailCB;      // b2

    const char* GetTypeName() const { return "Trail"; }
    void Reflect(IReflector& r) {
        r.Field("duration",           duration);
        r.Field("maxPoints",          maxPoints);
        r.Field("sampleInterval",     sampleInterval);
        r.Field("minVertexDist",      minVertexDist);
        r.Field("widthStart",         widthStart);
        r.Field("widthEnd",           widthEnd);
        r.Field("colorStart",         colorStart);
        r.Field("colorEnd",           colorEnd);
        r.Field("alignment",          alignment);
        r.Field("smoothSubdivisions", smoothSubdivisions);
        r.Field("texturePath",        texturePath);
        r.Field("uvScrollSpeed",      uvScrollSpeed);
        r.Field("uvTiling",           uvTiling);
    }
};

} // namespace fbzz::scene
```

---

## 頂点レイアウト

```cpp
struct TrailVertex {
    math::Vector3 position;  // ワールド空間マイタージョイント展開済み位置
    float         age;       // 0.0=最古（末尾）, 1.0=最新（先端） — GPU lerp 用
    float         v;         // リボン端: 0.0=上端, 1.0=下端
    float         u;         // トレイル進行方向 [0,1]（テクスチャ U 座標）
};
// 24 bytes/vertex（旧設計の 36 bytes から 33% 削減）
// WHY: カラー(float4=16B)を CPU で展開する代わりに age(float=4B) を渡し、
//      GPU が TrailConstants から colorStart/colorEnd を lerp する。
//      転送量削減に加え、色変更時に VB の再アップロードが不要になる。
```

---

## リングバッファ操作

### 初期化

```cpp
void TrailComponent_Init(TrailComponent& t) {
    t.pointBuffer.resize(t.maxPoints);
    t.ringHead = t.ringTail = t.ringCount = 0;
    t.lastSampleTime = -1.0f;
}
```

### 点の追加（System 内）

```cpp
void RingPushBack(TrailComponent& t, TrailPoint p) {
    if (t.ringCount == t.maxPoints) {
        // バッファ満杯: 最古点を上書き（head を進める）
        t.ringHead = (t.ringHead + 1) % t.maxPoints;
    } else {
        ++t.ringCount;
    }
    t.pointBuffer[t.ringTail] = p;
    t.ringTail = (t.ringTail + 1) % t.maxPoints;
}
```

### 古い点の削除（System 内）

```cpp
void RingExpireOld(TrailComponent& t, float currentTime) {
    while (t.ringCount > 0) {
        const TrailPoint& oldest = t.pointBuffer[t.ringHead];
        if (oldest.timestamp >= currentTime - t.duration) break;
        t.ringHead = (t.ringHead + 1) % t.maxPoints;
        --t.ringCount;
    }
}
```

### インデックスアクセス（System 内、i=0 が最古）

```cpp
inline TrailPoint& RingAt(TrailComponent& t, int i) {
    return t.pointBuffer[(t.ringHead + i) % t.maxPoints];
}
```

---

## 点列更新アルゴリズム

```
入力: TrailComponent& trail, Transform& transform, float currentTime

① 古い点の削除:
   RingExpireOld(trail, currentTime)

② 新点の追加条件（どちらも満たすとき）:
   A. currentTime - trail.lastSampleTime >= trail.sampleInterval
   B. ringCount == 0 || dist(transform.position, RingAt(trail, ringCount-1).position)
                         >= trail.minVertexDist

   WHY: A だけだと高 FPS で点が蓄積しすぎる。B だけだと低 FPS で点が飛ぶ（不均一）。
        両方の制約で「時間的に均等・空間的に最低密度」を保証する。

   条件を満たすとき:
       RingPushBack(trail, { transform.position, currentTime })
       trail.lastSampleTime = currentTime
```

---

## マイタージョイント法線計算

n 個の制御点から各点のジョイント法線を計算する。

```
for i in [0, n-1]:

    // 各セグメントの法線を計算
    if i > 0:
        segL = normalize(P[i] - P[i-1])
        nL   = computeNormal(segL, alignment, cameraPos, P[i])
    if i < n-1:
        segR = normalize(P[i+1] - P[i])
        nR   = computeNormal(segR, alignment, cameraPos, P[i])

    // 端点はセグメント法線をそのまま使用
    if   i == 0:   miter = nR
    elif i == n-1: miter = nL
    else:
        miter = normalize(nL + nR)

        // マイター長: 幅が一定になるよう補正
        // cos(halfAngle) = dot(miter, nL) なので length = width / cos(halfAngle)
        cosHalfAngle = dot(miter, nL)
        miterLength  = 1.0f / max(cosHalfAngle, 0.5f)  // クランプ: 急角度でリボンが広がりすぎない
        miter *= miterLength

    jointNormals[i] = miter
```

### 法線計算 `computeNormal`

```cpp
math::Vector3 computeNormal(
    const math::Vector3& segDir,
    TrailAlignment        alignment,
    const math::Vector3& cameraPos,
    const math::Vector3& pointPos)
{
    math::Vector3 up;
    if (alignment == TrailAlignment::CameraFacing) {
        up = math::Normalize(cameraPos - pointPos);
    } else {
        up = math::Vector3(0, 1, 0);
        // segDir が真上に近いとき (0,1,0) とのクロス積が退化する
        if (std::abs(math::Dot(segDir, up)) > 0.99f)
            up = math::Vector3(1, 0, 0);
    }
    return math::Normalize(math::Cross(segDir, up));
}
```

---

## リボン頂点展開

n 個の制御点から `(n-1) × 2` 個のトライアングル（= `(n-1) × 6` 頂点、TRIANGLE_LIST）。

```
for i in [0, n-2]:

    age_i    = (P[i].timestamp    - (currentTime - duration)) / duration  // [0,1]
    age_next = (P[i+1].timestamp  - (currentTime - duration)) / duration

    // リングバッファでは i=0 が最古なので age は昇順
    w_i    = lerp(widthEnd, widthStart, age_i)    * 0.5f
    w_next = lerp(widthEnd, widthStart, age_next) * 0.5f

    n_i    = jointNormals[i]
    n_next = jointNormals[i+1]

    u_i    = float(i)   / float(n - 1)
    u_next = float(i+1) / float(n - 1)

    TL = { P[i].position   + n_i    * w_i,    age_i,    0.0f, u_i    }
    BL = { P[i].position   - n_i    * w_i,    age_i,    1.0f, u_i    }
    TR = { P[i+1].position + n_next * w_next, age_next, 0.0f, u_next }
    BR = { P[i+1].position - n_next * w_next, age_next, 1.0f, u_next }

    // TRIANGLE_LIST: TL,TR,BL, BL,TR,BR
    emit TL, TR, BL, BL, TR, BR
```

---

## Catmull-Rom サブディビジョン（Phase 5、`smoothSubdivisions > 0` 時）

制御点をレンダリング前に補間して滑らかな曲線を生成する。
補間点は描画用の一時バッファに生成し、`pointBuffer` は変更しない。

```
for i in [0, n-2]:
    P0 = (i > 0)   ? RingAt(t, i-1).position : P[i].position * 2 - P[i+1].position
    P1 = RingAt(t, i).position
    P2 = RingAt(t, i+1).position
    P3 = (i < n-2) ? RingAt(t, i+2).position : P[i+1].position * 2 - P[i].position

    subdiv = smoothSubdivisions
    for s in [0, subdiv]:
        tParam = float(s) / float(subdiv + 1)
        // Catmull-Rom 公式 (alpha = 0.5)
        pos = 0.5 * ((2*P1) + (-P0+P2)*t + (2*P0-5*P1+4*P2-P3)*t² + (-P0+3*P1-3*P2+P3)*t³)
        age = lerp(age_i, age_next, tParam)
        emit as temporary TrailPoint
```

---

## 動的頂点バッファ更新

`Map(WriteDiscard)` で毎フレーム全頂点を上書きする。

```
初期化時:
    // maxPoints 点から最大 (maxPoints-1) セグメント × 6 頂点
    size_t capacity = (trail.maxPoints - 1) * 6 * sizeof(TrailVertex);
    trail.vertexBuffer = resources.CreateDynamicVertexBuffer(capacity, sizeof(TrailVertex));

フレーム毎:
    void* ptr = renderer.MapBuffer(trail.vertexBuffer, MapType::WriteDiscard);
    memcpy(ptr, vertices.data(), vertices.size() * sizeof(TrailVertex));
    renderer.UnmapBuffer(trail.vertexBuffer);
```

`CreateDynamicVertexBuffer` / `MapBuffer` は `ResourceManager` への追加 API。

---

## Trail.hlsl シェーダー

```hlsl
// Assets/Shaders/Material/Effects/Trail.hlsl
// PSO: ALPHA_BLEND + DEPTH_READ (DepthWrite=Off) + CULL_NONE

// TrailConstants は MaterialConstants スロット (CB_MATERIAL=b2) を占有する。
// WHY: Trail シェーダーは Material シェーダーと共存しないため、
//      同スロットを上書きして再利用することで既存の cbuffer スロット規約を崩さない。
#define FBZZ_MATERIAL_CONSTANTS
cbuffer TrailConstants : register(CB_MATERIAL)
{
    float4 colorStart;      // 最新点（先端）カラー
    float4 colorEnd;        // 最古点（末尾）カラー
    float  uvScrollSpeed;   // U 方向スクロール速度（単位/秒）
    float  uvTiling;        // U 方向タイリング回数
    float  time;            // Application::GetTime() — TrailRenderSystem が毎フレーム Upload
    float  _pad;
};

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"

Texture2D    gTrailTex : register(TEX_ALBEDO);  // t0
SamplerState gSampler  : register(SAMPLER_DEFAULT);  // s0

struct VSIn {
    float3 position : POSITION;
    float  age      : TEXCOORD0;  // 0=最古, 1=最新
    float  v        : TEXCOORD1;  // 0=上端, 1=下端
    float  u        : TEXCOORD2;  // トレイル進行方向 [0,1]
};

struct VSOut {
    float4 svPos : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float  age   : TEXCOORD1;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    // 位置は CPU 側でマイタージョイント展開済み。VS は ViewProj のみ適用。
    o.svPos = mul(float4(i.position, 1.0f), viewProjection);
    o.uv    = float2(i.u, i.v);
    o.age   = i.age;
    return o;
}

float4 PSMain(VSOut i) : SV_Target0
{
    // UV スクロール: U にタイリングを掛けてから時間オフセットを加算。
    // WHY: tiling を先に掛けることでスクロール速度がタイリング数に依存しない。
    float2 uv = float2(i.uv.x * uvTiling + time * uvScrollSpeed, i.uv.y);

    float4 tex   = gTrailTex.Sample(gSampler, uv);
    float4 color = lerp(colorEnd, colorStart, i.age);
    return tex * color;
    // colorEnd.a = 0 で末尾は自然に透明フェードアウト
}
```

---

## パイプラインステート

| 項目 | 設定値 | 理由 |
|---|---|---|
| Blend | `ALPHA_BLEND`（SrcAlpha / InvSrcAlpha） | 半透明フェード |
| DepthWrite | **Off** | 半透明物はデプス書き込みしない |
| DepthTest | On（`LESS_EQUAL`） | 不透明メッシュに隠れる |
| CullMode | **None**（両面描画） | カメラ真横から見ると裏面が露出する |
| Topology | `TRIANGLE_LIST` | |

---

## DrawCall 発行

```cpp
DrawCall dc;
dc.vertexBuffer      = trail.vertexBuffer;
dc.shader            = trail.shader;
dc.pipelineState     = trail.pipelineState;
dc.textures[0]       = trail.texture;       // TEX_ALBEDO (t0)
dc.constantBuffers[0] = cameraCB;           // b0: CameraConstants
dc.constantBuffers[2] = trail.trailCB;      // b2: TrailConstants
dc.vertexCount       = uploadedVertexCount;
dc.indexCount        = 0;                   // インデックスなし描画
dc.topology          = PrimitiveTopology::TRIANGLE_LIST;
dc.layer             = RenderLayer::TRANSPARENT_LAYER;
renderer.Submit(dc, resources);
```

`ObjectConstants (b1)` は **不使用**。
CPU 側でワールド変換済みの座標を頂点に持つため `world` 行列は不要。

---

## 定数バッファスロット割り当て（DrawCall.hpp コメントと同期）

| スロット | 用途 |
|---|---|
| `b0` | `CameraConstants`（ViewProj 行列） |
| `b2` | `TrailConstants`（colorStart / colorEnd / uvScrollSpeed / uvTiling / time） |
| `t0` | トレイルテクスチャ |
| `s0` | デフォルトサンプラー |

---

## 隣接ドキュメント

- [overview.md](overview.md) — システム全体概要・フェーズ計画・ファイル配置
- [../Water/rendering.md](../Water/rendering.md) — 半透明 DrawCall の先行実装例
