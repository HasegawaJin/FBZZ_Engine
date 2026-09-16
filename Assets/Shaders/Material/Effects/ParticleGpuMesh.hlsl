// FBZZ Engine
// Material/Effects/ParticleGpuMesh.hlsl | VS + PS
// GPU シミュレーションのメッシュパーティクル描画 (インスタンシング)
// PSO: SOLID or SOLID_NOCULL + ALPHA_BLEND/ADDITIVE + DEPTH_READ
//
// VS: 通常のメッシュ頂点を読み、SV_InstanceID で StructuredBuffer<GpuParticle> を引いて
//     位置・サイズ・回転から TRS を組み立てる。
//
// WHY: メッシュパーティクル (破片・瓦礫) は CPU 経路だと粒子 1 個につき DrawCall 1 本で、
//      MeshTrailRenderPass が emitter->particles を舐めて発行していた。
//      GPU シミュレーションでは CPU 側に粒子配列が存在しないので描きようがなく、
//      meshParticlePath があるだけで CPU へ縮退していた。
//      粒子データは既に StructuredBuffer にあるので、インスタンス描画 1 本へ畳める。

// 構造体・StructuredBuffer・定数バッファは契約ヘッダーが供給する。
// ビルボードではなくメッシュをインスタンス描画するので、VS は自前で書く。
#define FBZZ_PARTICLE_GPU
#define FBZZ_PARTICLE_CUSTOM_VS
#include "Material/Effects/ParticleMaterial.hlsli"
#include "Common/Structs.hlsli"

struct MeshParticlePSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
    float4 color      : COLOR;
};

// 回転はビルボード経路と揃えて「視線方向まわり」ではなくワールド Z 軸まわりに掛ける。
// WHY: CPU 経路 (MeshTrailRenderPass) が FORWARD 軸まわりの Quaternion で TRS を組んでおり、
//      ここを変えると同じ .vfx が CPU/GPU で違う向きに見える。
float3x3 MeshParticleRotation(float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return float3x3( c,   -s,  0.0f,
                     s,    c,  0.0f,
                     0.0f, 0.0f, 1.0f);
}

MeshParticlePSIn VSMain(VSInput v, uint instanceId : SV_InstanceID)
{
    // ソート有効時は描画順の instanceId 番目が指す粒子を引く (半透明の破片で効く)。
    uint pIdx = gGpuSortEnabled != 0u ? gSortedParticles[instanceId].y : instanceId;
    GpuParticle p = gParticles[pIdx];

    MeshParticlePSIn o;
    // 死亡粒子と範囲外は NDC の外へ飛ばしてラスタライザに捨てさせる。
    // インスタンス数は maxParticles 固定なので、生死の判定はここでしかできない。
    if (p.age >= p.lifetime || (gMaxParticles > 0u && pIdx >= gMaxParticles))
    {
        o.svPosition = float4(0.0f, 0.0f, -2.0f, 1.0f);
        o.uv         = (float2)0;
        o.normalWS   = float3(0.0f, 1.0f, 0.0f);
        o.color      = (float4)0;
        return o;
    }

    // Mesh Particle は billboard と違い 3 軸すべてを使える (CPU 経路と同じ扱い)。
    // sizeAxisScale の z は b2 に無いため、xy と等方な z=1 相当として size をそのまま使う。
    float3 scale = float3(p.size * gSizeAxisScaleX, p.size * gSizeAxisScaleY, p.size);
    float3x3 rotation = MeshParticleRotation(p.rotation);

    float3 localPos = v.position * scale;
    float3 worldPos = mul(localPos, rotation) + p.position;
    // 法線は非等方スケールでも大きく崩れないよう、回転だけ掛けて正規化し直す。
    float3 worldNormal = normalize(mul(v.normal, rotation));

    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv         = v.uv;
    o.normalWS   = worldNormal;
    o.color      = p.color;
    return o;
}

float4 PSMain(MeshParticlePSIn input) : SV_Target0
{
    float4 texel = gParticleTex.Sample(gSampler, input.uv);
    // アルファの取り出し方と色空間はビルボード経路と同じ規約に従う。
    // ここがずれると、同じ素材が billboard と mesh で違う抜き方・違う色で出る。
    float4 resolved = ResolveParticleAlbedo(texel, gEffectsFlags);
    float4 color = resolved * input.color * gTintColor;

    // 破片の立体感を出すための最小限のライティング。半球状の環境光を法線で振るだけで、
    // 影も IBL も通さない (パーティクルは大量に出るためピクセルコストを増やさない)。
    float hemisphere = saturate(input.normalWS.y * 0.5f + 0.5f);
    float shade = lerp(1.0f - gLightingStrength * 0.5f, 1.0f, hemisphere);
    color.rgb *= shade * gEmissiveScale;
    return color;
}
