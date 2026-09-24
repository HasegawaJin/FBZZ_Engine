/// @file    ParticleLighting.hlsli
/// @brief   パーティクルの陰影 — 6 方向ライトマップ・疑似法線の lit smoke・点光源 (クラスタ) をまとめる
/// @author  Hasegawa Jin
/// @date    2026-09-11
/// @note Particle.hlsl (CPU 経路) と ParticleGPU.hlsl (GPU 経路) は同じ ShadeParticle() を使う。
/// @note ParticleCommon.hlsli (GPU 経路は ParticleMaterial.hlsli 経由) を先に include する。
#ifndef PARTICLE_LIGHTING_HLSLI
#define PARTICLE_LIGHTING_HLSLI

/// @note FBZZ_PUNCTUAL_END の heatmap return は float4 なので、この float3 helper では無効にする。
/// @note 画面空間 AO と接触影は板の粒子へ適用しない。
#define FBZZ_PUNCTUAL_NO_DEBUG
#define FBZZ_NO_SCREEN_SHADING
#include "Rendering/Lighting.hlsli"

#include "Common/FroxelFogConstants.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note 6 方向ライトマップの Negative 側 (左 / 下 / 手前 / 発光マスク)。Positive は albedo (t0)。
FBZZ_TEX2D(gSixWayNegative, TEX_EMISSIVE_SLOT);
/// @note six-way directional luminance を ramp の色相へ戻す補助アトラス。
FBZZ_TEX2D(gSixWayAlbedoColor, TEX_PARTICLE_SIX_WAY_COLOR_SLOT);
/// @note six-way emission RGB。RGB は既に積分・ソフトニー済みで、W の scale だけを適用する。
FBZZ_TEX2D(gSixWayEmissionColor, TEX_PARTICLE_SIX_WAY_EMISSION_SLOT);
/// @note 拡散 IBL の照度キューブ。iblIntensity が 0 なら ambientColor を使う。
FBZZ_TEXCUBE(gParticleIrradiance, TEX_IBL_IRRADIANCE_SLOT);
/// @note フロクセル霧の積分済みボリューム (rgb = 視線に沿った散乱光 / a = 透過率)。
FBZZ_TEX3D_T(float4, gParticleFroxelFog, TEX_FROXEL_FOG_SLOT);
SamplerState gParticleLinearClamp : register(SAMPLER_LINEAR_CLAMP);

/// @note 不透明なサーフェスと同じ空の照度を受ける環境光。
/// @note 単一の return にして、展開される punctual-light macro 内の FXC X4000 を避ける。
float3 ParticleAmbient(float3 dir)
{
    float3 ambient = ambientColor;
    if (iblIntensity > 0.0f)
        ambient = gParticleIrradiance.SampleLevel(gParticleLinearClamp, dir, 0.0f).rgb * iblIntensity * iblDiffuseScale;
    return ambient;
}

/// @note 向きを持たない媒質 (疑似法線の煙・ボリュメトリック) の環境光。上下と水平の平均。
float3 ParticleAmbientIsotropic()
{
    if (iblIntensity <= 0.0f) return ambientColor;
    return (ParticleAmbient(float3(0.0f, 1.0f, 0.0f)) + ParticleAmbient(float3(0.0f, -1.0f, 0.0f))
          + ParticleAmbient(float3(1.0f, 0.0f, 0.0f)) + ParticleAmbient(float3(-1.0f, 0.0f, 0.0f))
          + ParticleAmbient(float3(0.0f, 0.0f, 1.0f)) + ParticleAmbient(float3(0.0f, 0.0f, -1.0f))) / 6.0f;
}

/// @note 各軸の環境光を対応する 6 方向マップで重み付けする。一様な空なら SixWayAmbient と一致する。
float3 SixWayAmbientIbl(float3 positive, float3 negative, float3 right, float3 up, float3 back)
{
    if (iblIntensity <= 0.0f) return ambientColor * SixWayAmbient(positive, negative);
    return (ParticleAmbient(right) * positive.r + ParticleAmbient(up) * positive.g + ParticleAmbient(back) * positive.b
          + ParticleAmbient(-right) * negative.r + ParticleAmbient(-up) * negative.g + ParticleAmbient(-back) * negative.b)
         / 6.0f;
}

/// @note 粒子描画は HDR 合成より先に行われるため、粒子深度の霧を背景深度で合成する結果へ補正する。
/// @note 粒子深度までの透過率と散乱を (Tp, Sp)、背景深度までを (Tb, Sb)、事前乗算色を c、alpha を a とする。
/// @note 正しい合成は Tp·(c + (1-a)·背景を粒子深度の霧へ通した値) + Sp となる。
/// @note Composite が計算する Tb·(c' + (1-a)·背景) + Sb に一致させるには c' = (Tp·c − a·(Sb − Sp)) / Tb。
/// @note 加算材質では c' = c·Tp / Tb。前方散乱が強いと c' は負になりうるが HDR 合成では有効。
float3 ApplyParticleFog(float3 premultipliedRgb, float alpha, float2 svXY, float particleNdcZ, float sceneNdcZ)
{
    if (froxelGridZ == 0u) return premultipliedRgb;
    const float2 uv = svXY / max(float2(gScreenWidth, gScreenHeight), float2(1.0f, 1.0f));
    const float particleZ = LinearizeDepth(particleNdcZ, nearZ, farZ, isOrthographic);
    const float sceneZ = LinearizeDepth(sceneNdcZ, nearZ, farZ, isOrthographic);
    const float4 atParticle = FBZZ_SampleIntegratedFroxel(gParticleFroxelFog, gParticleLinearClamp, uv, particleZ);
    const float4 atScene    = FBZZ_SampleIntegratedFroxel(gParticleFroxelFog, gParticleLinearClamp, uv, sceneZ);
    const float sceneTransmittance = max(atScene.a, 0.02f);
    if ((gEffectsFlags & FBZZ_PFX_ADDITIVE) != 0u)
        return premultipliedRgb * (atParticle.a / sceneTransmittance);
    return (premultipliedRgb * atParticle.a - alpha * (atScene.rgb - atParticle.rgb)) / sceneTransmittance;
}

/// @note PSMain の最後に呼ぶ。result は blend mode に対応した合成入力。
float4 FinishParticleFog(float4 result, float2 svXY, float particleNdcZ, float sceneNdcZ)
{
    const bool premultiplied = (gEffectsFlags & FBZZ_PFX_PREMULTIPLIED) != 0u;
    /// @note Additive は straight RGB なので、霧計算中だけ alpha を掛けてから戻す。
    /// @note 歪みは背景そのものを描き直すため、背景と同じ霧を使う。
    if (froxelGridZ == 0u || (gEffectsFlags & FBZZ_PFX_DISTORTION) != 0u) return result;
    const float3 premultipliedRgb = premultiplied ? result.rgb : result.rgb * result.a;
    const float3 fogged = ApplyParticleFog(premultipliedRgb, result.a, svXY, particleNdcZ, sceneNdcZ);
    result.rgb = premultiplied ? fogged : fogged / max(result.a, 1.0e-4f);
    return result;
}

/// @note Point / Spot / area light は各粒子の中心で測り、粒子板を跨ぐ光の輪を避ける。
/// @note 粒子板に法線は無いため N·L を含まない強度を使う。
/// @note sixWay では光の向きを各軸マップの応答で重み付けする。
float3 ParticlePunctualLight(ParticlePSIn p, float3 axisRight, float3 axisUp, float3 axisBack,
                             float3 positive, float3 negative, bool sixWay)
{
    float3 sum = 0.0f;
    const float3 toViewer = normalize(cameraPos - p.center);
    FBZZ_PUNCTUAL_BEGIN(p.center, p.svPosition.xy, toViewer)
        float response = 1.0f;
        if (sixWay)
            response = SixWayResponse(positive, negative,
                                      float3(dot(ps.L, axisRight), dot(ps.L, axisUp), dot(ps.L, axisBack)));
        sum += ps.color * (ps.intensityNoCosine * response);
    FBZZ_PUNCTUAL_END
    return sum;
}

/// @brief パーティクルへ陰影と six-way emission を適用する。
/// @param base テクスチャと粒子色を乗算した値。six-way map 使用時は参照しない。
/// @param positive six-way Positive/albedo サンプル。
/// @param negative six-way Negative/emission サンプル。
/// @param albedoColor six-way albedo hue atlas の線形色。
/// @param emissionColor 露出済み emission atlas の線形色。
/// @param shadow 平行光の受け影と自己影。
/// @return 陰影付きの線形 RGB。
float3 ShadeParticle(ParticlePSIn p, float3 base, float4 positive, float4 negative,
                     float4 albedoColor, float4 emissionColor, float shadow)
{
    const float3 toLight = normalize(-lightDir);
    const bool punctual = (gEffectsFlags & FBZZ_PFX_PUNCTUAL) != 0u;
    const bool premultiplied = (gEffectsFlags & FBZZ_PFX_PREMULTIPLIED) != 0u;
    const bool hasColorMaps = (gEffectsFlags & FBZZ_PFX_SIX_WAY_COLOR_MAPS) != 0u;
    float3 rgb;

    if ((gEffectsFlags & FBZZ_PFX_SIX_WAY_MAPS) != 0u)
    {
        const float3 viewRight = float3(view[0][0], view[1][0], view[2][0]);
        const float3 viewUp    = float3(view[0][1], view[1][1], view[2][1]);
        float3 axisRight, axisUp;
        ParticleTextureAxes(p.worldPos, p.localUv, viewRight, viewUp, axisRight, axisUp);
        /// @note 左手系なので右 × 上 = 奥。軸は板の実際の向きに従う。
        const float3 axisBack = normalize(cross(axisRight, axisUp));
        const float3 L = float3(dot(toLight, axisRight), dot(toLight, axisUp), dot(toLight, axisBack));
        float3 light = SixWayAmbientIbl(positive.rgb, negative.rgb, axisRight, axisUp, axisBack)
                     + lightColor * (SixWayResponse(positive.rgb, negative.rgb, L) * shadow);
        if (punctual)
            light += ParticlePunctualLight(p, axisRight, axisUp, axisBack, positive.rgb, negative.rgb, true);
        if (hasColorMaps)
        {
            const float3 albedo = max(albedoColor.rgb, 0.0f);
            const float albedoLuminance = dot(albedo, float3(0.2126f, 0.7152f, 0.0722f));
            light *= albedo / max(albedoLuminance, 1.0e-4f);
        }
        /// @note Fire の premultiplied 6-way 出力では smoke lighting だけ Positive alpha で覆い、emission は独立させる。
        if (premultiplied)
        {
            /// @note 新しい atlas は Negative alpha と別に積分済み。旧素材だけ emission mask とソフトニーを使う。
            const float3 fireEmission = hasColorMaps
                ? max(emissionColor.rgb, 0.0f) * max(gSixWayEmission.w, 0.0f)
                : 1.0f - exp(-max(gSixWayEmission.rgb, 0.0f) * negative.a);
            rgb = light * p.color.rgb * gTintColor.rgb * (positive.a * gTintColor.a)
                + fireEmission * gTintColor.a;
        }
        else
        {
            const float3 emission = hasColorMaps
                ? max(emissionColor.rgb, 0.0f) * max(gSixWayEmission.w, 0.0f)
                : gSixWayEmission.rgb * negative.a;
            rgb = light * p.color.rgb * gTintColor.rgb + emission;
        }
    }
    else if ((gEffectsFlags & FBZZ_PFX_SIX_WAY) != 0u)
    {
        /// @note ビルボード面を球とみなした疑似法線を作る。
        const float2 normalXY = p.localUv * 2.0f - 1.0f;
        const float3 normal = normalize(float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY)))));
        const float3 viewDir = normalize(cameraPos - p.worldPos);
        /// @note 巻き込み拡散と前方散乱で、煙が不透明な球に見えたり炎の手前が暗く残ったりするのを防ぐ。
        const float diffuse = ParticleWrappedDiffuse(dot(normal, toLight), saturate(gSmokeWrap));
        const float back = ParticleBackScatter(viewDir, toLight, gSmokeBackScatterPower, gSmokeTransmission);
        /// @note 影は直接光成分だけに掛け、環境光を残して煙が黒く潰れるのを防ぐ。
        float3 lit = ParticleAmbientIsotropic() + lightColor * ((diffuse + back) * shadow);
        if (punctual)
            lit += ParticlePunctualLight(p, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false);
        rgb = base * lerp(float3(1.0f, 1.0f, 1.0f), lit, saturate(gLightingStrength));
    }
    else
    {
        /// @note 非ライティング材質は色へ直接影を掛ける。発光体は strength の範囲で減衰させる。
        rgb = base * shadow;
        /// @note 素の色は照明済みとして扱い、点光源を加算して炎の近くの煙を明るくする。
        if (punctual)
            rgb += base * ParticlePunctualLight(p, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false) * saturate(gLightingStrength);
    }
    return rgb;
}

#endif
