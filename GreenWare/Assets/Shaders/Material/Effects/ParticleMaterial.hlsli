/// @file    ParticleMaterial.hlsli
/// @brief   パーティクル材質シェーダーの共通契約。カスタムシェーダーは必ずこれを include する
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY 契約をファイルにするか:
///   パーティクルは «頂点バッファのレイアウトを 1 つも間違えずに宣言し、カメラへ正対する
///   板へ展開する» という定型が絵の内容と無関係に必ず要る。これをシェーダーごとに
///   写経させると、電荷を 1 種類足すだけで 9 個の semantics と 29 個の cbuffer メンバーを
///   順序ごと書き写すことになり、1 つ違えても «出ない / 形が崩れる» 以外の情報が出ない。
///   定型は ParticleBillboardVS() に閉じ、材質側は PSMain だけ書く。
///
/// WHY 材質パラメータを MaterialConstants (b2) に置くか:
///   BuildDescriptor() は cbuffer を **名前** で探す
///   (GetConstantBufferByName("MaterialConstants"))。この名前とレジスタに合わせておく
///   だけで、既存の .mat / ShaderDescriptor / Inspector がそのままパーティクルにも効く。
///   エンジンが埋めるビルボード情報は CB_PARTICLE (b11) へ逃がしてある。
///
/// 使い方 (最小):
/// @code
///   #include "Material/Effects/ParticleMaterial.hlsli"
///
///   cbuffer MaterialConstants : register(CB_MATERIAL)
///   {
///       float4 coreColor;   // .mat の [params] と **名前** で結ばれる
///       float  filaments;
///   };
///
///   // VSMain はこのヘッダーが供給する。書くのは PSMain だけ。
///   float4 PSMain(ParticlePSIn p) : SV_Target0
///   {
///       float d = length(p.localUv - 0.5f);
///       return float4(coreColor.rgb * exp(-d * filaments), 1.0f) * p.color;
///   }
/// @endcode
///
/// オプション (include より前に定義する):
///   FBZZ_PARTICLE_GPU        … GPU シミュレーション用 VS を供給する
///                              (頂点バッファではなく StructuredBuffer から引く)。
///                              PSMain は CPU 経路とそのまま共用できる。
///   FBZZ_PARTICLE_CUSTOM_VS  … 既定の VSMain を供給しない。自分で VSMain を書く場合に定義する。
///
/// 使えるテクスチャスロット:
///   t0 = .mat の [textures] albedo (このヘッダーが gParticleTex として宣言済み)
///   t2 / t3 / t4 は空き。**t1 / t5〜t9 はパーティクルパスが占有している**
///   (歪みマップ / シーン色 / モーションベクター / 深度 / 影 / 自己影密度)。
///   .mat の tex6 / tex7 はパーティクルでは届かない。
#ifndef FBZZ_PARTICLE_MATERIAL_HLSLI
#define FBZZ_PARTICLE_MATERIAL_HLSLI

// b11 の ParticleRenderConstants / ParticleVSIn / ParticlePSIn / ParticleBillboardVS。
// Constants.hlsli より先に include すること (b2 を材質へ空けるため)。
#include "Rendering/ParticleCommon.hlsli"

Texture2D    gParticleTex : register(TEX_ALBEDO);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

#ifdef FBZZ_PARTICLE_GPU

// LAYOUT: Material/Effects/ParticleGpuSim.cs.hlsl の GpuParticle と
//         Engine/Scene/Components/ParticleEmitter.hpp の GpuParticle (112 bytes) に一致させること。
struct GpuParticle
{
    float3 position;
    float  size;
    float3 velocity;
    float  age;
    float4 color;
    float  lifetime;
    float  rotation;
    float  angularVelocity;
    float  spriteSeed;
    float4 uvRect;
    // 色ゆらぎ倍率。VS では読まないが、StructuredBuffer の stride を
    // 112 バイトへ合わせるため必ず宣言する。
    float3 colorScale;
    float  spriteBlend;   // 次のコマへの補間率 (Frame Blending)
    float4 nextUvRect;    // 次のコマの UV 矩形
};

StructuredBuffer<GpuParticle> gParticles       : register(SB_GPU_PARTICLES);
// GPU ソート結果 (key, particleIndex)。gGpuSortEnabled が 0 のときは何もバインドされない。
// WHY: 半透明は描画順で結果が変わるため、粒子プールの並び順ではなくカメラ距離で
//      並べ替えた順に描く必要がある。プール自体は並べ替えない
//      (リングバッファの位置が動くとスポーンとシミュレーションが破綻する)。
StructuredBuffer<uint2>       gSortedParticles : register(SB_GPU_SORT);

static const float2 FBZZ_PARTICLE_QUAD_CORNERS[6] =
{
    float2(-0.5f,  0.5f),  // TL
    float2( 0.5f,  0.5f),  // TR
    float2(-0.5f, -0.5f),  // BL
    float2( 0.5f,  0.5f),  // TR (2nd tri)
    float2( 0.5f, -0.5f),  // BR
    float2(-0.5f, -0.5f),  // BL
};

static const float2 FBZZ_PARTICLE_QUAD_UVS[6] =
{
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f),
    float2(0.0f, 1.0f),
};

// 生きている GPU 粒子 1 つぶんの、corner 番目のビルボード頂点。
ParticlePSIn ParticleGpuExpandBillboard(GpuParticle p, uint corner)
{
    ParticlePSIn o;

    // カメラ空間 X/Y 軸のワールド向き (row-major view 行列の列 0, 1)
    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up    = float3(view[0][1], view[1][1], view[2][1]);
    float lengthScale = 1.0f;
    if (gRenderMode == 1)
    {
        float speed = length(p.velocity);
        if (speed > 1.0e-4f)
        {
            up = p.velocity / speed;
            float3 viewDir = normalize(cameraPos - p.position);
            right = normalize(cross(up, viewDir));
            lengthScale = max(gStretchedLengthScale + speed * gStretchedVelocityScale, 0.0f);
        }
    }
    else if (gRenderMode == 2)
    {
        right = float3(1.0f, 0.0f, 0.0f);
        up = float3(0.0f, 0.0f, 1.0f);
    }
    else if (gRenderMode == 3)
    {
        up = float3(0.0f, 1.0f, 0.0f);
        float3 viewDir = normalize(cameraPos - p.position);
        right = normalize(cross(up, viewDir));
    }

    float2 localUv = FBZZ_PARTICLE_QUAD_UVS[corner];
    float2 c       = FBZZ_PARTICLE_QUAD_CORNERS[corner];

    // 回転適用
    float s  = sin(p.rotation);
    float fc = cos(p.rotation);
    c = float2(c.x * fc - c.y * s, c.x * s + c.y * fc);

    // 軸ごとの倍率は回転の後に掛ける (ParticleBillboardVS と同じ順序。CPU/GPU で見た目を揃える)。
    float3 worldPos = p.position
                    + right * c.x * p.size * gSizeAxisScaleX
                    + up    * c.y * p.size * gSizeAxisScaleY * lengthScale;

    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv         = lerp(p.uvRect.xy, p.uvRect.zw, localUv);
    o.localUv    = localUv;
    o.nextUv     = lerp(p.nextUvRect.xy, p.nextUvRect.zw, localUv);
    o.spriteBlend = p.spriteBlend;
    o.worldPos   = worldPos;
    o.center     = p.position;
    // 非等方スケール時は大きい方の半径を採用する。クワッドの半幅は size * 0.5 * 軸倍率
    // (QUAD_CORNERS が ±0.5) なので、半径にも 0.5 が要る。
    o.radius     = p.size * 0.5f * max(gSizeAxisScaleX, gSizeAxisScaleY);
    o.color      = p.color;
    return o;
}

// GPU 粒子 1 つぶんのビルボード展開。頂点バッファは無く SV_VertexID から引く。
// Draw(6 * maxParticles, 0) で呼ぶ: vertId / 6 = 粒子番号、vertId % 6 = 三角形の頂点。
//
// WHY CPU 経路と同じ ParticlePSIn を返すか: 材質側の PSMain を CPU / GPU で
//     そのまま共用できるようにするため。次のコマと補間率は CS が粒子ごとに書いている。
// WHY 展開を別関数にするか: 死亡粒子の分岐で早期 return すると FXC が X4000 を出す。
ParticlePSIn ParticleGpuBillboardVS(uint vertId)
{
    uint slot   = vertId / 6;
    uint corner = vertId % 6;

    // ソート有効時は「描画順の slot 番目」が指す粒子を引く。
    // 死亡粒子と詰め物は最大キーで末尾へ落ちており、その index は maxParticles 以上か
    // age >= lifetime なので、下の棄却判定にそのまま吸収される。
    uint pIdx = gGpuSortEnabled != 0u ? gSortedParticles[slot].y : slot;

    GpuParticle p = gParticles[pIdx];

    ParticlePSIn o;
    if (p.age >= p.lifetime || (gMaxParticles > 0 && pIdx >= gMaxParticles))
    {
        // 死亡粒子: クリップ空間外に出力してラスタライザが棄却するようにする
        o = (ParticlePSIn)0;
        o.svPosition = float4(0.0f, 0.0f, -2.0f, 1.0f); // z=-2 → NDC 外
    }
    else
    {
        o = ParticleGpuExpandBillboard(p, corner);
    }
    return o;
}

#endif // FBZZ_PARTICLE_GPU

#ifndef FBZZ_PARTICLE_CUSTOM_VS
#ifdef FBZZ_PARTICLE_GPU
ParticlePSIn VSMain(uint vertId : SV_VertexID) { return ParticleGpuBillboardVS(vertId); }
#else
ParticlePSIn VSMain(ParticleVSIn v) { return ParticleBillboardVS(v); }
#endif
#endif // FBZZ_PARTICLE_CUSTOM_VS

#endif // FBZZ_PARTICLE_MATERIAL_HLSLI
