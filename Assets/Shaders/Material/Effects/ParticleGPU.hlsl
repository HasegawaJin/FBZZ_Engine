// FBZZ Engine
// Material/Effects/ParticleGPU.hlsl | VS + PS
// GPU パーティクル billboard 描画シェーダー
// PSO: SOLID_NOCULL + ADDITIVE/ALPHA_BLEND + DEPTH_READ
//
// VS: StructuredBuffer<GpuParticle> から SV_VertexID でパーティクルを取り出し、
//     ビュー行列の右/上ベクトルでビルボードを展開する。
//     age >= lifetime の粒子は w=0 の NaN clip 座標を出力して棄却する。
//
// 頂点レイアウト: 頂点バッファなし。Draw(6 * maxParticles, 0) で呼ぶ。
//   SV_VertexID / 6 = パーティクルインデックス
//   SV_VertexID % 6 = クワッドの三角形頂点インデックス

#include "Common/Binding.hlsli"
#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"

// ---------- 構造体 --------------------------------------------------------

struct GpuParticle
{
    float3 position;
    float  size;
    float3 velocity;
    float  age;
    float4 color;
    float  lifetime;
    float  rotation;
    float  pad0;
    float  pad1;
    float4 uvRect;
};

// ---------- リソース -------------------------------------------------------

StructuredBuffer<GpuParticle> gParticles : register(SB_GPU_PARTICLES);
Texture2D                     gTex       : register(TEX_ALBEDO);
SamplerState                  gSampler   : register(SAMPLER_DEFAULT);

// ---------- VS / PS 間 ---------------------------------------------------

struct PsIn
{
    float4 svPos  : SV_POSITION;
    float2 uv     : TEXCOORD0;
    float2 localUv: TEXCOORD1;
    float4 color  : COLOR;
};

// ---------- VS ------------------------------------------------------------

static const float2 QUAD_CORNERS[6] =
{
    float2(-0.5f,  0.5f),  // TL
    float2( 0.5f,  0.5f),  // TR
    float2(-0.5f, -0.5f),  // BL
    float2( 0.5f,  0.5f),  // TR (2nd tri)
    float2( 0.5f, -0.5f),  // BR
    float2(-0.5f, -0.5f),  // BL
};

static const float2 QUAD_UVS[6] =
{
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f),
    float2(0.0f, 1.0f),
};

PsIn VSMain(uint vertId : SV_VertexID)
{
    uint  pIdx   = vertId / 6;
    uint  corner = vertId % 6;

    GpuParticle p = gParticles[pIdx];

    PsIn o;

    // 死亡粒子: クリップ空間外に出力してラスタライザが棄却するようにする
    if (p.age >= p.lifetime)
    {
        o.svPos   = float4(0.0f, 0.0f, -2.0f, 1.0f); // z=-2 → NDC 外
        o.uv      = (float2)0;
        o.localUv = (float2)0;
        o.color   = (float4)0;
        return o;
    }

    // カメラ空間 X/Y 軸のワールド向き (row-major view 行列の列 0, 1)
    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up    = float3(view[0][1], view[1][1], view[2][1]);

    float2 localUv = QUAD_UVS[corner];
    float2 c       = QUAD_CORNERS[corner];

    // 回転適用
    float s = sin(p.rotation);
    float fc = cos(p.rotation);
    c = float2(c.x * fc - c.y * s, c.x * s + c.y * fc);

    float3 worldPos = p.position
                    + right * c.x * p.size
                    + up    * c.y * p.size;

    o.svPos   = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv      = lerp(p.uvRect.xy, p.uvRect.zw, localUv);
    o.localUv = localUv;
    o.color   = p.color;
    return o;
}

// ---------- PS ------------------------------------------------------------

float4 PSMain(PsIn p) : SV_Target0
{
    // 中心から外側にかけてソフトフェード
    float2 d    = p.localUv * 2.0f - 1.0f;
    float  fade = saturate(1.0f - dot(d, d));
    fade *= fade;
    float4 tex = gTex.Sample(gSampler, p.uv);
    return tex * float4(p.color.rgb, p.color.a * fade);
}
