// FBZZ Engine
// PostProcess/Color/Composite.hlsl | PostProcess
// 最終合成パス — HDR + Bloom を合成してトーンマップ・ガンマ補正し LDR に出力する
// フォグ: 深度バッファから線形距離を復元して指数フォグを適用する

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/ToneMap.hlsli"
#include "Rendering/Fog.hlsli"
#include "Rendering/Atmosphere.hlsli"
#include "Rendering/PostProcess.hlsli"

Texture2D          texHDR      : register(TEX_GBUFFER0);        // ライティング結果 HDR バッファ
Texture2D          texBloom    : register(TEX_BLOOM);
Texture2D<float>   texDepth    : register(TEX_DEPTH);           // 深度 (フォグ計算用)
Texture3D          texLUT      : register(TEX_LUT_COLOR_GRADE); // 3D カラーグレーディング LUT (32x32x32 推奨)
// ---- Advanced Graphics ----
// SSR 反射 — ssrIntensity > 0 のとき alpha チャンネルをブレンド係数として HDR に乗せる
Texture2D<float4>  texSSR        : register(TEX_SSR);
// Volumetric Light — volLightIntensity > 0 のとき HDR に加算合成する
Texture2D<float4>  texVolumetric : register(TEX_VOLUMETRIC);
SamplerState       sampDefault     : register(SAMPLER_DEFAULT);
SamplerState       sampLinearClamp : register(SAMPLER_LINEAR_CLAMP); // LUT サンプル用（テクセル中心補間に必須）

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv         = float2((id & 1u) ? 2.0f : 0.0f,
                          (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float LinearDepth(float2 uv)
{
    float ndcZ = texDepth.Sample(sampDefault, uv).r;
    if (ndcZ >= 0.9999f)
        return farZ;

    return nearZ * farZ / (farZ - ndcZ * (farZ - nearZ));
}

float3 SampleHdrWithBloom(float2 uv)
{
    float3 color = texHDR.Sample(sampDefault, uv).rgb;
    color += texBloom.Sample(sampDefault, uv).rgb * bloomIntensity;
    return color;
}

float3 ApplyDepthOfFieldHDR(float3 hdr, float2 uv)
{
    if (dofBlurRadius <= 0.0f)
        return hdr;

    // WHAT: 焦点距離から離れたピクセルほど、固定 8 点サンプルのぼかしを強く混ぜる。
    // WHY: 専用 CoC バッファを増やさない軽量版として、Composite 内で完結させる。
    float depth = LinearDepth(uv);
    float blur = saturate(abs(depth - dofFocusDistance) / max(dofFocusRange, 0.001f));
    float2 radius = texelSize * dofBlurRadius * blur;

    float3 sum = hdr;
    sum += SampleHdrWithBloom(saturate(uv + float2( radius.x,  0.0f)));
    sum += SampleHdrWithBloom(saturate(uv + float2(-radius.x,  0.0f)));
    sum += SampleHdrWithBloom(saturate(uv + float2( 0.0f,  radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2( 0.0f, -radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2( radius.x,  radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2(-radius.x,  radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2( radius.x, -radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2(-radius.x, -radius.y)));

    return lerp(hdr, sum / 9.0f, blur);
}

float3 ApplySharpenHDR(float3 hdr, float2 uv)
{
    if (sharpenStrength <= 0.0f)
        return hdr;

    // WHAT: 十字 4 近傍の平均を引いたアンシャープマスク。
    // WHY: Sobel のような輪郭抽出より安価で、Bloom 後の HDR に自然な解像感を足せる。
    float2 r = texelSize * max(sharpenRadius, 0.25f);
    float3 blur =
        SampleHdrWithBloom(saturate(uv + float2( r.x, 0.0f))) +
        SampleHdrWithBloom(saturate(uv + float2(-r.x, 0.0f))) +
        SampleHdrWithBloom(saturate(uv + float2(0.0f,  r.y))) +
        SampleHdrWithBloom(saturate(uv + float2(0.0f, -r.y)));
    blur *= 0.25f;

    return max(hdr + (hdr - blur) * sharpenStrength, 0.0f);
}

float3 ApplyClarity(float3 ldr, float2 uv)
{
    if (clarityStrength <= 0.0f)
        return ldr;

    // WHAT: 少し広い近傍平均との差分を LDR に戻すローカルコントラスト補正。
    // WHY: シャープ化より大きい面の明暗差を強調し、ディテールが眠い画を自然に引き締める。
    float2 r = texelSize * max(clarityRadius, 0.5f);
    float3 blur =
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2( r.x, 0.0f))), exposure) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(-r.x, 0.0f))), exposure) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(0.0f,  r.y))), exposure) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(0.0f, -r.y))), exposure);
    blur *= 0.25f;

    return saturate(ldr + (ldr - blur) * clarityStrength);
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float2 sourceUV = ApplyPixelateUV(p.uv, pixelSize);
    float2 uv = LensDistortUV(sourceUV, lensDistortion);
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float2 caOffset = (uv - 0.5f) * chromaticAberration;
    float3 hdr;
    hdr.r = texHDR.Sample(sampDefault, uv + caOffset).r;
    hdr.g = texHDR.Sample(sampDefault, uv).g;
    hdr.b = texHDR.Sample(sampDefault, uv - caOffset).b;
    float3 bloom = texBloom.Sample(sampDefault, uv).rgb;
    hdr += bloom * bloomIntensity;
    hdr = ApplyDepthOfFieldHDR(hdr, uv);
    hdr = ApplySharpenHDR(hdr, uv);

    // ---- Advanced Graphics: SSR 反射をトーンマップ前 (HDR 空間) でブレンドする ----
    // WHY: HDR 空間でブレンドすることで金属の映り込みが過露出部分でも正しく飽和する。
    //      ssrIntensity=0 のときはテクスチャが未束縛でも 0 を返すため分岐不要。
    if (ssrIntensity > 0.0f)
    {
        float4 ssrSample = texSSR.Sample(sampDefault, uv);
        // alpha は Fresnel・roughness を含む信頼度。強度はここで一度だけ適用する。
        hdr = lerp(hdr, ssrSample.rgb, saturate(ssrSample.a * ssrIntensity));
    }
    // ---- Volumetric Light を HDR に加算合成する ----
    // WHY: 加算なので暗い領域に光の筋が自然に乗り、tonemapper がクランプする。
    if (volLightIntensity > 0.0f)
    {
        float3 volSample = texVolumetric.Sample(sampDefault, uv).rgb;
        hdr += volSample * volLightIntensity;
    }

    // 露出 → ACES トーンマップ → sRGB ガンマ補正
    float3 ldr = FinalOutput(hdr, exposure);
    ldr = ApplyClarity(ldr, uv);

    // 深度から線形距離を復元してフォグを適用する。
    // ndcZ ≥ 0.9999 はスカイドーム（clip.xyww で z=w → NDC z=1.0）なので霧を掛けない。
    // フォグの適用ロジック (ApplyFog) は共通で、色の出どころだけ fogSource で切り替える (§3-3)。
    if (fogDensity > 0.0f)
    {
        float ndcZ = texDepth.Sample(sampDefault, uv).r;
        if (ndcZ < 0.9999f)
        {
            float linDepth = LinearDepth(uv);
            float dist     = max(linDepth - fogFar, 0.0f);
            float factor   = FogFactor(dist, fogDensity);

            float3 col = fogColor; // Exponential: 固定フォグ色
            if (fogSource > 0.5f)
            {
                // Atmosphere (エアリアルパースペクティブ): 視線方向の大気 in-scatter をフォグ色に使う。
                // 太陽から離れた遠景は青く、太陽方向は暖色に霞む。空ドームの見た目と整合する。
                float3 worldPos  = ReconstructWorldPos(uv, ndcZ, invViewProjection);
                float3 rayDir    = normalize(worldPos - cameraPos);
                float3 sunDir    = normalize(-lightDir);
                float  scaled    = sunIntensity * lightIntensity;
                float3 inscatter = ComputeAtmosphericScattering(
                    rayDir, sunDir, rayleighScattering, mieScattering, mieG, scaled) * lightColor;
                // フォグは LDR 空間で適用するため、in-scatter (HDR) を露出→トーンマップして合わせる。
                col = FinalOutput(inscatter, exposure);
            }
            ldr = ApplyFog(ldr, factor, col);
        }
    }

    // 水没カメラ用の全画面補正。
    // WHY: 水面そのものは Water パスで描くが、カメラが水中に入った時の吸収・濁り・視界歪みは
    //      画面全体にかかる効果なので Composite で一括処理する。
    if (underwaterStrength > 0.0f)
    {
        float depthFactor = underwaterStrength;
        float ndcZ = texDepth.Sample(sampDefault, uv).r;
        if (ndcZ < 0.9999f)
        {
            float linDepth = LinearDepth(uv);
            depthFactor = saturate((1.0f - exp(-underwaterFogDensity * linDepth)) * underwaterStrength);
        }

        float wave = sin((uv.x + uv.y) * 28.0f + time * 2.4f) * 0.003f * underwaterStrength;
        float2 distortedUV = saturate(uv + float2(wave, wave * 0.5f));
        float3 distortedHdr = texHDR.Sample(sampDefault, distortedUV).rgb;
        float3 distortedLdr = FinalOutput(distortedHdr, exposure);

        ldr = lerp(ldr, distortedLdr, underwaterStrength * 0.25f);
        ldr = lerp(ldr, underwaterColor, depthFactor);

        float gray = dot(ldr, float3(0.299f, 0.587f, 0.114f));
        ldr = lerp(float3(gray, gray, gray), ldr, lerp(1.0f, 0.55f, underwaterStrength));
    }

    ldr = ApplyColorAdjustments(ldr, contrast, saturation, hueShift, temperature, tint);
    ldr = ApplyShadowHighlight(ldr, shadowLift, highlightCompression);
    ldr = ApplyColorFilter(ldr, colorFilter, colorFilterIntensity);
    ldr = ApplySepia(ldr, sepiaIntensity);
    ldr = ApplyInvert(ldr, invertIntensity);
    ldr = ApplyPosterize(ldr, posterizeLevels);
    ldr = ApplyVignette(ldr, uv, vignetteIntensity, vignetteSmoothness, vignetteRoundness, vignetteColor);
    ldr = ApplyFilmGrain(ldr, uv, filmGrainIntensity, filmGrainResponse);

    // 3D LUT カラーグレーディング — フィルムグレイン・ビネットなど全エフェクト後に適用する。
    // WHY: LUT はシネマティックな色調整（フィルムエミュレーション等）を 1 テクスチャルックアップで
    //      表現できるため、個別パラメータの積み重ねより表現力が高い。
    //      全エフェクト後に適用することで LUT が意図した最終カラーに確実に変換する。
    if (lutBlend > 0.0f)
    {
        // 0.5/lutSize オフセットでテクセル中心をサンプルする
        // WHY: 3D LUT は離散テクセルなので端に寄せると境界クランプが起きる
        //      (lutSize - 1) / lutSize でデータ範囲を 0〜1 にマップし、
        //      0.5 / lutSize でテクセル中心にオフセットする
        static const float lutSize = 32.0f;
        float3 lutUV   = saturate(ldr) * ((lutSize - 1.0f) / lutSize) + (0.5f / lutSize);
        float3 lutColor = texLUT.Sample(sampLinearClamp, lutUV).rgb;
        ldr = lerp(ldr, lutColor, saturate(lutBlend));
    }

    // 画面フェード — 全エフェクト適用後の最終合成として上書きする。
    // WHY: UI・ポストプロセス含む全レイヤーをひとつの lerp でカバーし、シーン遷移時のフラッシュを防ぐ。
    if (screenFadeAlpha > 0.0f)
        ldr = lerp(ldr, screenFadeColor, saturate(screenFadeAlpha));

    return float4(ldr, 1.0f);
}
