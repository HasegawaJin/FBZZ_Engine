/// @file    ParticleLighting.hlsli
/// @brief   パーティクルの陰影 — 6 方向ライトマップ・疑似法線の lit smoke・点光源 (クラスタ) をまとめる
/// @author  Hasegawa Jin
/// @date    2026-09-11
//
// Particle.hlsl (CPU 経路) と ParticleGPU.hlsl (GPU 経路) が同じ ShadeParticle() で陰影を付ける。
// 前提: ParticleCommon.hlsli (GPU 経路は ParticleMaterial.hlsli 経由) を先に include しておく。
#ifndef PARTICLE_LIGHTING_HLSLI
#define PARTICLE_LIGHTING_HLSLI

// FBZZ_PUNCTUAL_END のヒートマップ表示は float4 を返して打ち切る作りで、float3 を返す
// ここでは文法的に通らない。画面空間 AO / 接触影は板の粒子には意味が無いので受け口ごと外す。
#define FBZZ_PUNCTUAL_NO_DEBUG
#define FBZZ_NO_SCREEN_SHADING
#include "Rendering/Lighting.hlsli"

#include "Common/FroxelFogConstants.hlsli"
#include "Common/BindlessIndices.hlsli"

// 6 方向ライトマップの Negative 側 (左 / 下 / 手前 / 発光マスク)。Positive は albedo (t0)。
FBZZ_TEX2D(gSixWayNegative, TEX_EMISSIVE_SLOT);
// 拡散 IBL (空連動の照度キューブ)。iblIntensity が 0 のフレームは読まず ambientColor へ落ちる。
FBZZ_TEXCUBE(gParticleIrradiance, TEX_IBL_IRRADIANCE_SLOT);
// フロクセル霧の積分済みボリューム (rgb = 視線に沿って散乱してきた光 / a = 透過率)。
FBZZ_TEX3D_T(float4, gParticleFroxelFog, TEX_FROXEL_FOG_SLOT);
SamplerState gParticleLinearClamp : register(SAMPLER_LINEAR_CLAMP);

// dir の向きから来る環境光。不透明なサーフェスと同じ «空の照度» を受けないと、
// 同じ場所に置いた煙だけが灰色に浮く。
// NOTE: 出口を 1 つにしてある。ループ内で展開される関数の早期 return は FXC が X4000 で咎める。
float3 ParticleAmbient(float3 dir)
{
    float3 ambient = ambientColor;
    if (iblIntensity > 0.0f)
        ambient = gParticleIrradiance.SampleLevel(gParticleLinearClamp, dir, 0.0f).rgb * iblIntensity * iblDiffuseScale;
    return ambient;
}

// 向きを持たない媒質 (疑似法線の煙・ボリュメトリック) の環境光。上下と水平の平均。
float3 ParticleAmbientIsotropic()
{
    if (iblIntensity <= 0.0f) return ambientColor;
    return (ParticleAmbient(float3(0.0f, 1.0f, 0.0f)) + ParticleAmbient(float3(0.0f, -1.0f, 0.0f))
          + ParticleAmbient(float3(1.0f, 0.0f, 0.0f)) + ParticleAmbient(float3(-1.0f, 0.0f, 0.0f))
          + ParticleAmbient(float3(0.0f, 0.0f, 1.0f)) + ParticleAmbient(float3(0.0f, 0.0f, -1.0f))) / 6.0f;
}

// 6 方向マップの環境光。各軸の照度をその軸のマップで受ける (一様な空なら SixWayAmbient と一致)。
float3 SixWayAmbientIbl(float3 positive, float3 negative, float3 right, float3 up, float3 back)
{
    if (iblIntensity <= 0.0f) return ambientColor * SixWayAmbient(positive, negative);
    return (ParticleAmbient(right) * positive.r + ParticleAmbient(up) * positive.g + ParticleAmbient(back) * positive.b
          + ParticleAmbient(-right) * negative.r + ParticleAmbient(-up) * negative.g + ParticleAmbient(-back) * negative.b)
         / 6.0f;
}

// フロクセル霧を «粒子の奥行きで» 効かせるための補正。
// WHY: 霧は Composite が HDR 全体へ «背景の奥行き» で掛ける。粒子はその前に HDR へ描かれるので、
//     手前の煙まで背景と同じ濃さの霧に沈み、遠くの霧に浮く煙は霧の中から抜け出して見える。
//     粒子の奥行きまでの霧 (Tp, Sp) と背景までの霧 (Tb, Sb) から、Composite が掛けたあとで
//     正しい値になる出力を逆算して書く。
//       正: Tp·(c + (1-a)·(背景を粒子の奥で霧に通したもの)) + Sp
//       Composite: Tb·(c' + (1-a)·背景) + Sb
//     → 事前乗算 c' = (Tp·c − a·(Sb − Sp)) / Tb、加算 c' = c·Tp / Tb。
// NOTE: 手前の霧が明るいほど c' は負になりうる。HDR は浮動小数なので合成はそのまま正しく働く。
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

// PSMain の最後に呼ぶ。result は合成へ渡す値 (事前乗算ならそのまま、それ以外はストレート)。
float4 FinishParticleFog(float4 result, float2 svXY, float particleNdcZ, float sceneNdcZ)
{
    const bool premultiplied = (gEffectsFlags & FBZZ_PFX_PREMULTIPLIED) != 0u;
    const bool additive = (gEffectsFlags & FBZZ_PFX_ADDITIVE) != 0u;
    // 歪みは背景そのものを描き直しているので、背景と同じ霧が正しい。
    if (froxelGridZ == 0u || (gEffectsFlags & FBZZ_PFX_DISTORTION) != 0u) return result;
    const float3 premultipliedRgb = premultiplied || additive ? result.rgb : result.rgb * result.a;
    const float3 fogged = ApplyParticleFog(premultipliedRgb, result.a, svXY, particleNdcZ, sceneNdcZ);
    result.rgb = premultiplied || additive ? fogged : fogged / max(result.a, 1.0e-4f);
    return result;
}

// 点光源 (Point / Spot / 面光源) を粒子の中心で集める。
// WHY 中心で測るか: 画素ごとに測ると、光源が板を跨ぐときに板の上へ «光の輪» が描かれる。
//     煙の 1 粒は «一様に照らされる小さな雲» として扱う方が見た目が正しい。
// 板には法線が無いので N·L を掛けない強度 (intensityNoCosine) を使う。
// sixWay なら各光の向きを 6 方向マップの応答で重み付けする (axis* はテクスチャの軸)。
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

// 陰影を付けた RGB を返す。
//   base     … テクスチャ × 粒子色 × tint (6 方向マップのときは使わない。マップは色ではなく明るさなので)
//   positive … 6 方向マップの Positive (= albedo のサンプル)。FBZZ_PFX_SIX_WAY_MAPS のときだけ意味を持つ
//   negative … 6 方向マップの Negative (t3)
//   shadow   … 平行光への受け影 × 自己影
// NOTE: 出口を 1 つにしてある (ParticleAmbient と同じ理由)。
float3 ShadeParticle(ParticlePSIn p, float3 base, float4 positive, float4 negative, float shadow)
{
    const float3 toLight = normalize(-lightDir);
    const bool punctual = (gEffectsFlags & FBZZ_PFX_PUNCTUAL) != 0u;
    float3 rgb;

    if ((gEffectsFlags & FBZZ_PFX_SIX_WAY_MAPS) != 0u)
    {
        const float3 viewRight = float3(view[0][0], view[1][0], view[2][0]);
        const float3 viewUp    = float3(view[0][1], view[1][1], view[2][1]);
        float3 axisRight, axisUp;
        ParticleTextureAxes(p.worldPos, p.localUv, viewRight, viewUp, axisRight, axisUp);
        // 左手系なので右 × 上 = 奥 (画面の向こう)。板の実際の向きに従う。
        const float3 axisBack = normalize(cross(axisRight, axisUp));
        const float3 L = float3(dot(toLight, axisRight), dot(toLight, axisUp), dot(toLight, axisBack));
        float3 light = SixWayAmbientIbl(positive.rgb, negative.rgb, axisRight, axisUp, axisBack)
                     + lightColor * (SixWayResponse(positive.rgb, negative.rgb, L) * shadow);
        if (punctual)
            light += ParticlePunctualLight(p, axisRight, axisUp, axisBack, positive.rgb, negative.rgb, true);
        // 発光は光の当たり方と無関係 (炎の芯は影の中でも光る)。
        rgb = light * p.color.rgb * gTintColor.rgb + gSixWayEmission.rgb * negative.a;
    }
    else if ((gEffectsFlags & FBZZ_PFX_SIX_WAY) != 0u)
    {
        // ビルボードには本物の法線が無いため、スプライト面を球とみなした疑似法線を作る。
        const float2 normalXY = p.localUv * 2.0f - 1.0f;
        const float3 normal = normalize(float3(normalXY, sqrt(saturate(1.0f - dot(normalXY, normalXY)))));
        const float3 viewDir = normalize(cameraPos - p.worldPos);
        // 巻き込み拡散 + 前方散乱。素の N·L だけでは煙が「不透明な球」に見え、
        // 背後の光を透かさないので炎の手前の煙が暗いまま残る。
        const float diffuse = ParticleWrappedDiffuse(dot(normal, toLight), saturate(gSmokeWrap));
        const float back = ParticleBackScatter(viewDir, toLight, gSmokeBackScatterPower, gSmokeTransmission);
        // 影は直接光成分だけに掛け、環境光は残す (影の中の煙が真っ黒に潰れない)。
        float3 lit = ParticleAmbientIsotropic() + lightColor * ((diffuse + back) * shadow);
        if (punctual)
            lit += ParticlePunctualLight(p, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false);
        rgb = base * lerp(float3(1.0f, 1.0f, 1.0f), lit, saturate(gLightingStrength));
    }
    else
    {
        // 非ライティング時は色へ直接掛ける。発光体 (加算) では影が効きすぎないよう
        // 完全な 0 にはせず、strength の範囲で減衰させる。
        rgb = base * shadow;
        // 素の色は «既に照らされた色» なので、点光源は上乗せする (炎の近くの煙が明るくなる)。
        if (punctual)
            rgb += base * ParticlePunctualLight(p, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false) * saturate(gLightingStrength);
    }
    return rgb;
}

#endif // PARTICLE_LIGHTING_HLSLI
