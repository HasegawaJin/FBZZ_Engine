/// @file    ParticleGPU.hlsl
/// @brief   GPU パーティクル billboard 描画シェーダー (VS + PS)
/// @author  Hasegawa Jin
/// @date    2026-06-14
//
// PSO: SOLID_NOCULL + ADDITIVE/ALPHA_BLEND + DEPTH_READ
//
// VS: StructuredBuffer<GpuParticle> から SV_VertexID でパーティクルを取り出し、
//     ビュー行列の右/上ベクトルでビルボードを展開する。
//     age >= lifetime の粒子は NDC 外へ出して棄却する。
//
// 頂点レイアウト: 頂点バッファなし。Draw(6 * maxParticles, 0) で呼ぶ。
//   SV_VertexID / 6 = パーティクルインデックス
//   SV_VertexID % 6 = クワッドの三角形頂点インデックス
//
// 構造体・定数バッファ・VS は Material/Effects/ParticleMaterial.hlsli が供給する。
// CPU 経路 (Particle.hlsl) と同じ ParticlePSIn を受け取るため、PS の内容も揃う。

#define FBZZ_PARTICLE_GPU
#include "Material/Effects/ParticleMaterial.hlsli"
#include "Common/Space.hlsli"
#include "Rendering/ParticleNoise.hlsli"
#include "Rendering/ParticleSelfShadow.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/ParticleLighting.hlsli"
#include "Common/BindlessIndices.hlsli"

// 歪みベクトル専用ノーマルマップ (CPU 経路 Particle.hlsl と同じスロット・同じ扱い)。
FBZZ_TEX2D(gDistortionTex, TEX_NORMAL_SLOT);
FBZZ_TEX2D(gSceneDepth, TEX_DEPTH_SLOT);
FBZZ_TEX2D(gSceneColor, 5);
FBZZ_TEX2D(gMotionVectors, 6);
// 自己影の光源側密度 (R=Σα, G=Σα·深度)。CPU 経路と同じ t9 を使う。
FBZZ_TEX2D(gParticleDensity, TEX_PARTICLE_DENSITY_SLOT);
// 受け影用。CPU 経路 (Particle.hlsl) と同じスロット・同じ ComputeShadow を使う。
FBZZ_TEX2D_T(float, gShadowMap, TEX_SHADOW_SLOT);
SamplerComparisonState        gSampShadow: register(SAMPLER_SHADOW);

// ---------- PS ------------------------------------------------------------

float4 PSMain(ParticlePSIn p) : SV_Target0
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
        // 空の照度 (IBL) を受ける。定数の ambientColor だと、同じ場所の地面と煙で環境光が食い違う。
        const float3 volumetricAmbient = ParticleAmbientIsotropic();

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
            float3 inScatter = (volumetricAmbient
                + lightColor * (phase * lightTransmittance * mapShadow))
                * p.color.rgb * gTintColor.rgb;
            scattered += transmittance * (1.0f - stepTransmittance) * inScatter;
            transmittance *= stepTransmittance;
            if (transmittance < 0.01f) break;
        }

        float volAlpha = (1.0f - transmittance) * p.color.a;
        float particleLinear = LinearizeDepth(p.svPosition.z, nearZ, farZ, isOrthographic);
        if (gSoftParticles != 0)
        {
            float sceneDepth = gSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r;
            float sceneLinear = LinearizeDepth(sceneDepth, nearZ, farZ, isOrthographic);
            volAlpha *= saturate((sceneLinear - particleLinear) / gSoftParticleFadeDistance);
        }
        // CPU 経路 Particle.hlsl と同じ位置 (割り戻しより前) に掛ける。
        volAlpha *= ParticleCameraFade(particleLinear);
        // Particle.hlsl と同じ扱い: 積分済みの scattered は事前乗算なので、
        // PREMULTIPLIED 以外では割り戻して非事前乗算へ揃える。
        float3 volumeRgb = scattered * gEmissiveScale;
        if ((gEffectsFlags & FBZZ_PFX_PREMULTIPLIED) == 0u)
            volumeRgb /= max(volAlpha, 1.0e-4f);
        return FinishParticleFog(float4(volumeRgb, volAlpha), p.svPosition.xy, p.svPosition.z,
                                 gSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r);
    }

    // CPU 経路と同じ関数で 2 コマを混ぜる (次のコマと補間率は CS が書いている)。
    float2 currentUv;
    float4 tex = SampleParticleFlipbook(gParticleTex, gMotionVectors, gSampler,
                                        p.uv, p.nextUv, p.spriteBlend, gEffectsFlags, currentUv);
    float particleLinear = LinearizeDepth(p.svPosition.z, nearZ, farZ, isOrthographic);
    if (gSoftParticles != 0)
    {
        float sceneDepth = gSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r;
        float sceneLinear = LinearizeDepth(sceneDepth, nearZ, farZ, isOrthographic);
        fade *= saturate((sceneLinear - particleLinear) / gSoftParticleFadeDistance);
    }
    // CPU 経路と同じく fade へ掛ける (事前乗算では RGB にも同じ係数が掛かる)。
    fade *= ParticleCameraFade(particleLinear);
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

    // 陰影は CPU 経路と同じ ParticleLighting.hlsli で付ける。
    float4 sixWayNegative = 0.0f;
    float4 sixWayAlbedoColor = 0.0f;
    float4 sixWayEmissionColor = 0.0f;
    if ((gEffectsFlags & FBZZ_PFX_SIX_WAY_MAPS) != 0u)
    {
        float2 negativeUv;
        sixWayNegative = SampleParticleFlipbook(gSixWayNegative, gMotionVectors, gSampler, p.uv, p.nextUv,
                                                p.spriteBlend, gEffectsFlags & FBZZ_PFX_MOTION_VECTOR, negativeUv);
    }
    if ((gEffectsFlags & FBZZ_PFX_SIX_WAY_COLOR_MAPS) != 0u)
    {
        float2 colorUv;
        sixWayAlbedoColor = SampleParticleFlipbookColor(gSixWayAlbedoColor, gMotionVectors, gSampler,
            p.uv, p.nextUv, p.spriteBlend, gEffectsFlags & FBZZ_PFX_MOTION_VECTOR, colorUv);
        sixWayEmissionColor = SampleParticleFlipbookColor(gSixWayEmissionColor, gMotionVectors, gSampler,
            p.uv, p.nextUv, p.spriteBlend, gEffectsFlags & FBZZ_PFX_MOTION_VECTOR, colorUv);
    }
    result.rgb = ShadeParticle(p, result.rgb, tex, sixWayNegative, sixWayAlbedoColor, sixWayEmissionColor, shadow);
    result.rgb *= gEmissiveScale;
    if ((gEffectsFlags & FBZZ_PFX_DISTORTION) != 0u)
    {
        float2 screenUv = p.svPosition.xy / max(float2(gScreenWidth, gScreenHeight), float2(1.0f, 1.0f));
        // 専用マップがあればそちらを向きに使う (CPU 経路と同じ扱い)。
        float2 vector2 = (gEffectsFlags & FBZZ_PFX_DISTORTION_MAP) != 0u
            ? gDistortionTex.Sample(gSampler, currentUv).rg
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
    // 霧は Composite が背景の奥行きで掛ける。粒子の奥行きで効くよう、ここで逆算しておく。
    return FinishParticleFog(result, p.svPosition.xy, p.svPosition.z,
                             gSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r);
}
