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
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleCommon.hlsli"
#include "Rendering/ParticleNoise.hlsli"
#include "Rendering/ParticleSelfShadow.hlsli"
#include "Rendering/Shadow.hlsli"

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
    float  angularVelocity;
    float  spriteSeed;
    float4 uvRect;
    // 色ゆらぎ倍率。VS では読まないが、StructuredBuffer の stride を
    // ParticleGpuSim.cs.hlsl / GpuParticle (96 bytes) と一致させるため必ず宣言する。
    float3 colorScale;
    float  colorScalePad;
};

// ---------- リソース -------------------------------------------------------

StructuredBuffer<GpuParticle> gParticles : register(SB_GPU_PARTICLES);
// GPU ソート結果 (key, particleIndex)。gGpuSortEnabled が 0 のときは何もバインドされない。
// WHY: 半透明は描画順で結果が変わるため、粒子プールの並び順ではなく
//      カメラ距離で並べ替えた順に描く必要がある。プール自体は並べ替えない
//      (リングバッファの位置が動くとスポーンとシミュレーションが破綻する)。
StructuredBuffer<uint2>       gSortedParticles : register(SB_GPU_SORT);
Texture2D                     gTex       : register(TEX_ALBEDO);
// 歪みベクトル専用ノーマルマップ (CPU 経路 Particle.hlsl と同じスロット・同じ扱い)。
Texture2D                     gDistortionTex : register(TEX_NORMAL);
Texture2D                     gSceneDepth: register(TEX_DEPTH);
Texture2D                     gSceneColor: register(t5);
// 自己影の光源側密度 (R=Σα, G=Σα·深度)。CPU 経路と同じ t9 を使う。
Texture2D                     gParticleDensity : register(TEX_PARTICLE_DENSITY);
// 受け影用。CPU 経路 (Particle.hlsl) と同じスロット・同じ ComputeShadow を使う。
Texture2D<float>              gShadowMap : register(TEX_SHADOW);
SamplerState                  gSampler   : register(SAMPLER_DEFAULT);
SamplerComparisonState        gSampShadow: register(SAMPLER_SHADOW);

cbuffer ParticleRenderConstants : register(CB_MATERIAL)
{
    uint  gRenderMode;
    float gStretchedVelocityScale;
    float gStretchedLengthScale;
    float gSoftParticleFadeDistance;
    uint  gSoftParticles;
    uint  gMaxParticles;
    uint  gEffectsFlags;
    float gDistortionStrength;
    float gLightingStrength;
    float gEmissiveScale;
    float gMotionVectorStrength;
    // 描画先の解像度。PostProcConstants の screenSize はパーティクル描画では
    // バインドされないため、歪みの画面UVはこちらを使う。
    float gScreenWidth;
    float gScreenHeight;
    // ビルボードの軸ごとサイズ倍率 (Particle.hlsl の CPU 経路と同じ値・同じ意味)
    float gSizeAxisScaleX;
    float gSizeAxisScaleY;
    // 受け影の強さ [0,1]。0 で無効。
    float gShadowStrength;
    // ボリュメトリック煙 (gEffectsFlags bit4)。CPU 経路 Particle.hlsl と同じ意味。
    uint  gVolumetricSteps;
    float gVolumetricDensity;
    float gVolumetricAnisotropy;
    float gVolumetricNoiseScale;
    // 1 = gSortedParticles にソート済みインデックスが入っている (GPU ソート有効)。
    uint  gGpuSortEnabled;
    // 自己影の消衰係数。0 で無効。
    float gSelfShadowStrength;
    // 以降は CPU 経路 Particle.hlsl と同じ並び。GeometryPasses.hpp の ParticleRenderCB が正本。
    float gSmokeWrap;
    float gSmokeTransmission;
    float4 gTintColor;
    float gSmokeBackScatterPower;
    float gDistortionChromatic;
    float gParticlePad1;
    float gParticlePad2;
};

// ---------- ボリュメトリック煙 (Particle.hlsl と同じ式) ---------------------

float HenyeyGreenstein(float cosTheta, float g)
{
    float gg = g * g;
    float denom = 1.0f + gg - 2.0f * g * cosTheta;
    return (1.0f - gg) / (4.0f * 3.14159265f * max(pow(abs(denom), 1.5f), 1.0e-4f));
}

float VolumetricDensityAt(float3 samplePos, float3 center, float radius)
{
    float3 offset = (samplePos - center) / max(radius, 1.0e-4f);
    float  r = length(offset);
    if (r >= 1.0f) return 0.0f;
    float falloff = 1.0f - r;
    falloff *= falloff;
    float noise = FbmNoise3D(samplePos * gVolumetricNoiseScale, 3);
    return saturate(falloff * (0.6f + 0.8f * noise));
}

// ---------- VS / PS 間 ---------------------------------------------------

struct PsIn
{
    float4 svPos  : SV_POSITION;
    float2 uv     : TEXCOORD0;
    float2 localUv: TEXCOORD1;
    // 受け影のシャドウマップ投影に使うワールド座標。
    float3 worldPos: TEXCOORD2;
    // ボリュメトリック煙のレイマーチ用: 粒子中心と半径 (ワールド単位)。
    float3 center  : TEXCOORD3;
    float  radius  : TEXCOORD4;
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
    uint  slot   = vertId / 6;
    uint  corner = vertId % 6;

    // ソート有効時は「描画順の slot 番目」が指す粒子を引く。
    // 死亡粒子と詰め物は最大キーで末尾へ落ちており、その index は maxParticles 以上か
    // age >= lifetime なので、下の棄却判定にそのまま吸収される。
    uint pIdx = gGpuSortEnabled != 0u ? gSortedParticles[slot].y : slot;

    GpuParticle p = gParticles[pIdx];

    PsIn o;

    // 死亡粒子: クリップ空間外に出力してラスタライザが棄却するようにする
    if (p.age >= p.lifetime || (gMaxParticles > 0 && pIdx >= gMaxParticles))
    {
        o.svPos    = float4(0.0f, 0.0f, -2.0f, 1.0f); // z=-2 → NDC 外
        o.uv       = (float2)0;
        o.localUv  = (float2)0;
        o.worldPos = (float3)0;
        o.center   = (float3)0;
        o.radius   = 0.0f;
        o.color    = (float4)0;
        return o;
    }

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

    float2 localUv = QUAD_UVS[corner];
    float2 c       = QUAD_CORNERS[corner];

    // 回転適用
    float s = sin(p.rotation);
    float fc = cos(p.rotation);
    c = float2(c.x * fc - c.y * s, c.x * s + c.y * fc);

    // 軸ごとの倍率は回転の後に掛ける (Particle.hlsl と同じ順序。CPU/GPU で見た目を揃える)。
    float3 worldPos = p.position
                    + right * c.x * p.size * gSizeAxisScaleX
                    + up    * c.y * p.size * gSizeAxisScaleY * lengthScale;

    o.svPos    = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv       = lerp(p.uvRect.xy, p.uvRect.zw, localUv);
    o.localUv  = localUv;
    o.worldPos = worldPos;
    o.center   = p.position;
    // CPU 経路と同じく、非等方スケール時は大きい方の半径を採用する
    // (GPU 経路のクワッドは 0.5 込みなので p.size をそのまま半径として扱う)。
    o.radius   = p.size * max(gSizeAxisScaleX, gSizeAxisScaleY);
    o.color    = p.color;
    return o;
}

// ---------- PS ------------------------------------------------------------

float4 PSMain(PsIn p) : SV_Target0
{
    // 中心から外側にかけてソフトフェード
    float2 d    = p.localUv * 2.0f - 1.0f;
    float  fade = RadialMask(p.localUv);

    // ── ボリュメトリック煙 (CPU 経路 Particle.hlsl と同じ手順・同じ式) ──
    if ((gEffectsFlags & FBZZ_PFX_VOLUMETRIC) != 0u)
    {
        float discSq = dot(d, d);
        if (discSq >= 1.0f) discard;

        float3 viewDir = normalize(cameraPos - p.center);
        float3 right = float3(view[0][0], view[1][0], view[2][0]);
        float3 up    = float3(view[0][1], view[1][1], view[2][1]);

        float halfChord = sqrt(saturate(1.0f - discSq)) * p.radius;
        float3 entry = p.center
                     + right * (d.x * p.radius)
                     + up    * (d.y * p.radius)
                     + viewDir * halfChord;

        uint  steps = max(gVolumetricSteps, 1u);
        float stepLength = (2.0f * halfChord) / (float)steps;
        float3 lightDirection = normalize(-lightDir);
        float  phase = HenyeyGreenstein(dot(-viewDir, lightDirection), gVolumetricAnisotropy);

        float3 scattered = 0.0f;
        float  transmittance = 1.0f;
        [loop] for (uint s = 0; s < steps; ++s)
        {
            float3 samplePos = entry - viewDir * (((float)s + 0.5f) * stepLength);
            float density = VolumetricDensityAt(samplePos, p.center, p.radius);
            if (density <= 0.0f) continue;

            float shadowDensity = 0.0f;
            [unroll] for (int ls = 1; ls <= 2; ++ls)
            {
                float3 lightSample = samplePos + lightDirection * (p.radius * 0.4f * (float)ls);
                shadowDensity += VolumetricDensityAt(lightSample, p.center, p.radius);
            }
            float lightTransmittance = exp(-shadowDensity * gVolumetricDensity);

            float mapShadow = 1.0f;
            if ((gEffectsFlags & FBZZ_PFX_RECEIVE_SHADOW) != 0u)
            {
                mapShadow = ComputeShadow(gShadowMap, gSampShadow, samplePos,
                                          lightViewProjection, shadowMapTexelSize, shadowBias,
                                          lightDirection, lightDirection);
                mapShadow = lerp(1.0f, mapShadow, saturate(gShadowStrength));
            }
            // 雲全体の自己影 (CPU 経路 Particle.hlsl と同じ扱い)。
            // 粒子 1 個の球内部だけでは出ない「雲を貫く光の筋」がこれで現れる。
            // NOTE: GPU 経路では密度バッファが用意されない (自己影は CPU 縮退する) ため、
            //       実際には gSelfShadowStrength が 0 で素通りする。式は CPU と揃えておく。
            mapShadow *= ComputeParticleSelfShadowFromMap(
                gParticleDensity, gSampler, samplePos, lightViewProjection, gSelfShadowStrength);

            float extinction = density * gVolumetricDensity * stepLength;
            float stepTransmittance = exp(-extinction);
            float3 inScatter = (ambientColor
                + lightColor * (phase * lightTransmittance * mapShadow))
                * p.color.rgb * gTintColor.rgb;
            scattered += transmittance * (1.0f - stepTransmittance) * inScatter;
            transmittance *= stepTransmittance;
            if (transmittance < 0.01f) break;
        }

        float volAlpha = (1.0f - transmittance) * p.color.a;
        if (gSoftParticles != 0)
        {
            float sceneDepth = gSceneDepth.Load(int3(int2(p.svPos.xy), 0)).r;
            float sceneLinear = LinearizeDepth(sceneDepth, nearZ, farZ);
            float particleLinear = LinearizeDepth(p.svPos.z, nearZ, farZ);
            volAlpha *= saturate((sceneLinear - particleLinear) / gSoftParticleFadeDistance);
        }
        return float4(scattered * gEmissiveScale, volAlpha);
    }

    // 素材の作り (アルファ付き / 黒背景 / 白背景 / R マスク) を吸収し、RGB をリニアへ揃える。
    float4 tex = ResolveParticleAlbedo(gTex.Sample(gSampler, p.uv), gEffectsFlags);
    if (gSoftParticles != 0)
    {
        float sceneDepth = gSceneDepth.Load(int3(int2(p.svPos.xy), 0)).r;
        float sceneLinear = LinearizeDepth(sceneDepth, nearZ, farZ);
        float particleLinear = LinearizeDepth(p.svPos.z, nearZ, farZ);
        fade *= saturate((sceneLinear - particleLinear) / gSoftParticleFadeDistance);
    }
    // 頂点カラー (グラデーション) は既にリニア。tint は .mat 由来の共有色調整。
    float4 result = tex * float4(p.color.rgb * gTintColor.rgb, p.color.a * fade * gTintColor.a);

    // 受け影 (CPU 経路 Particle.hlsl と同じ扱い。ビルボードは法線を持たないため
    // 法線バイアスは無効化する)。
    float shadow = 1.0f;
    if ((gEffectsFlags & FBZZ_PFX_RECEIVE_SHADOW) != 0u)
    {
        float3 lightDirection = normalize(-lightDir);
        shadow = ComputeShadow(gShadowMap, gSampShadow, p.worldPos,
                               lightViewProjection, shadowMapTexelSize, shadowBias,
                               lightDirection, lightDirection);
        shadow = lerp(1.0f, shadow, saturate(gShadowStrength));
    }
    // 自己影 (CPU 経路と同じ密度バッファ・同じ式)。粒子群が自分に落とす影。
    shadow *= ComputeParticleSelfShadowFromMap(gParticleDensity, gSampler, p.worldPos,
                                               lightViewProjection, gSelfShadowStrength);

    if ((gEffectsFlags & FBZZ_PFX_SIX_WAY) != 0u)
    {
        // 巻き込み拡散 + 前方散乱 (CPU 経路 Particle.hlsl と同じ式)。
        float2 normalXY = p.localUv * 2.0f - 1.0f;
        float3 normal = normalize(float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY)))));
        float3 lightDirection = normalize(-lightDir);
        float3 viewDir = normalize(cameraPos - p.worldPos);
        float diffuse = ParticleWrappedDiffuse(dot(normal, lightDirection), saturate(gSmokeWrap));
        float back = ParticleBackScatter(viewDir, lightDirection,
                                         gSmokeBackScatterPower, gSmokeTransmission);
        float3 lit = ambientColor + lightColor * ((diffuse + back) * shadow);
        result.rgb *= lerp(float3(1.0f, 1.0f, 1.0f), lit, saturate(gLightingStrength));
    }
    else
    {
        result.rgb *= shadow;
    }
    result.rgb *= gEmissiveScale;
    if ((gEffectsFlags & FBZZ_PFX_DISTORTION) != 0u)
    {
        float2 screenUv = p.svPos.xy / max(float2(gScreenWidth, gScreenHeight), float2(1.0f, 1.0f));
        // 専用マップがあればそちらを向きに使う (CPU 経路と同じ扱い)。
        float2 vector2 = (gEffectsFlags & FBZZ_PFX_DISTORTION_MAP) != 0u
            ? gDistortionTex.Sample(gSampler, p.uv).rg
            : tex.rg;
        float2 offset = (vector2 * 2.0f - 1.0f) * gDistortionStrength;
        float2 dispersion = offset * gDistortionChromatic;
        float3 refracted;
        refracted.r = gSceneColor.Sample(gSampler, saturate(screenUv + offset + dispersion)).r;
        refracted.g = gSceneColor.Sample(gSampler, saturate(screenUv + offset)).g;
        refracted.b = gSceneColor.Sample(gSampler, saturate(screenUv + offset - dispersion)).b;
        result = float4(refracted, result.a);
    }
    // 事前乗算アルファは SrcBlend=ONE なので RGB が「そのまま」出力される。
    // 他のブレンドはブレンド側が src.a を掛けてくれるため RGB は素のままでよいが、
    // 事前乗算では RGB 自身が alpha 込みの値になっていなければならない。
    // 掛け落としていた 2 つはどちらも致命的だった:
    //   RadialMask (fade の初期値) — 矩形の角を消すマスク。RGB に掛からないと角が
    //     残り、スプライトが「■」として見える。
    //   p.color.a — グラデーションのアルファ。RGB に掛からないと、寿命の終わりで
    //     alpha が 0 へ落ちても RGB が残り続け、消えるはずの粒子が四角い光として
    //     居座る (「途中から■が見え始める」の正体)。
    // 歪み (distortion) で RGB を差し替えた後にも効かせる必要があるため、
    // 個々の分岐ではなく PSMain 末尾の 1 か所へ置く。
    // NOTE: ボリュメトリック経路は scattered を alpha で重み付け済みのまま早期 return
    //       するので、ここは通らない (二重に掛からない)。
    if ((gEffectsFlags & FBZZ_PFX_PREMULTIPLIED) != 0u) result.rgb *= p.color.a * fade;
    return result;
}
