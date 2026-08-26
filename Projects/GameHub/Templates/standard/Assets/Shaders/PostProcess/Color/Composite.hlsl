// FBZZ Engine
// PostProcess/Color/Composite.hlsl | PostProcess
// 最終合成パス — HDR + Bloom を合成してトーンマップ・ガンマ補正し LDR に出力する
// フォグ: 深度バッファから線形距離を復元して指数フォグを適用する

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ToneMap.hlsli"
#include "Rendering/Fog.hlsli"
#include "Rendering/Atmosphere.hlsli"
#include "Rendering/PostProcess.hlsli"
#include "Common/FroxelFogConstants.hlsli"

Texture2D          texHDR      : register(TEX_GBUFFER0);        // ライティング結果 HDR バッファ
Texture2D          texBloom    : register(TEX_BLOOM);
Texture2D<float>   texDepth    : register(TEX_DEPTH);           // 深度 (フォグ計算用)
Texture3D          texLUT      : register(TEX_LUT_COLOR_GRADE); // 3D カラーグレーディング LUT (32x32x32 推奨)
// ---- Advanced Graphics ----
// SSR 反射 — ssrIntensity > 0 のとき alpha チャンネルをブレンド係数として HDR に乗せる
Texture2D<float4>  texSSR        : register(TEX_SSR);
// Volumetric Light — volLightIntensity > 0 のとき HDR に加算合成する
Texture2D<float4>  texVolumetric : register(TEX_VOLUMETRIC);
// 全画面フェッチはすべてこれ 1 本で引く。
// WHY s0 (SAMPLER_DEFAULT) を使わないか: DX12 の静的サンプラーでは s0 が WRAP
//     (メッシュテクスチャのタイリング用) になっている。色収差やレンズ歪みは uv を
//     画面外へずらすので、s0 で引くと画面端が反対側の端を読み、上下左右がつながる。
SamplerState       sampLinearClamp : register(SAMPLER_LINEAR_CLAMP);

// ---- フロクセル ボリューメトリック フォグ ----
// froxelGridZ == 0 のとき無効。b13 が未束縛なら全ゼロで読まれるので、そのまま素通りする。
Texture3D<float4>  texFroxelFog : register(TEX_FROXEL_FOG);

// ---- 自動露出 ----
// [0] = 順応済みの平均輝度。ExposureAverage.cs.hlsl が毎フレーム書く。
// autoExposureKey <= 0 のときは束縛されておらず、b5 の exposure をそのまま使う。
StructuredBuffer<float> gAdaptedLuminance : register(SB_PUNCTUAL_LIGHTS); // t29

// ResolveExposure — b5 の手動 exposure と自動露出を 1 か所で合流させる。
//
// WHY 中間グレーで割るか: 露出とは「平均輝度を中間グレーへ持ってくる倍率」。
//     0.18 は反射率 18% のグレーカード、つまり写真の露出計が基準にしている明るさ。
float ResolveExposure()
{
    if (autoExposureKey <= 0.0f)
        return exposure;

    const float avg = max(gAdaptedLuminance[0], 1e-5f);
    float ev = log2(autoExposureKey / avg) + autoExposureCompensation;
    ev = clamp(ev, autoExposureMinEV, autoExposureMaxEV);
    // 手動 exposure は自動露出の上に乗る倍率として残す。絵作りの最終調整に使える。
    return exposure * exp2(ev);
}

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float LinearDepthFromNdc(float ndcZ)
{
    if (ndcZ >= 0.9999f)
        return farZ;

    return nearZ * farZ / (farZ - ndcZ * (farZ - nearZ));
}

// ApplyFroxelFog — 積分済みボリュームから「加算する光」と「背景の透過率」を取り出す。
//   rgb = 視線に沿って散乱してきた光, a = 背景に掛ける透過率
float3 ApplyFroxelFog(float3 hdr, float2 uv, float ndcDepth)
{
    if (froxelGridZ == 0u) return hdr;

    // 深度が最遠 (スカイドーム) のときは、グリッドの最終スライスまで積分した値になる。
    const float viewZ = LinearDepthFromNdc(ndcDepth);
    const float slice = FBZZ_FroxelViewZToSlice(viewZ);

    // Load ではなく Sample。スライス間を補間しないと、粗いグリッドの境界が
    // そのまま画面上の縞になる。
    const float3 volumeUV = float3(uv, (slice + 0.5f) / float(froxelGridZ));
    const float4 fog = texFroxelFog.SampleLevel(sampLinearClamp, volumeUV, 0);

    return hdr * fog.a + fog.rgb;
}

float3 SampleHdrWithBloom(float2 uv)
{
    float3 color = texHDR.Sample(sampLinearClamp, uv).rgb;
    [branch]
    if (bloomIntensity > 0.0f)
        color += texBloom.Sample(sampLinearClamp, uv).rgb * bloomIntensity;
    return color;
}

float3 ApplyDepthOfFieldHDR(float3 hdr, float2 uv, float ndcDepth)
{
    if (dofBlurRadius <= 0.0f)
        return hdr;

    // WHAT: 焦点距離から離れたピクセルほど、固定 8 点サンプルのぼかしを強く混ぜる。
    // WHY: 専用 CoC バッファを増やさない軽量版として、Composite 内で完結させる。
    float depth = LinearDepthFromNdc(ndcDepth);
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
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2( r.x, 0.0f))), ResolveExposure()) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(-r.x, 0.0f))), ResolveExposure()) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(0.0f,  r.y))), ResolveExposure()) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(0.0f, -r.y))), ResolveExposure());
    blur *= 0.25f;

    return saturate(ldr + (ldr - blur) * clarityStrength);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float2 sourceUV = ApplyPixelateUV(p.uv, pixelSize);
    float2 uv = LensDistortUV(sourceUV, lensDistortion);
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 hdr;
    [branch]
    if (abs(chromaticAberration) > 1.0e-5f)
    {
        const float2 caOffset = (uv - 0.5f) * chromaticAberration;
        hdr.r = texHDR.Sample(sampLinearClamp, uv + caOffset).r;
        hdr.g = texHDR.Sample(sampLinearClamp, uv).g;
        hdr.b = texHDR.Sample(sampLinearClamp, uv - caOffset).b;
    }
    else
    {
        hdr = texHDR.Sample(sampLinearClamp, uv).rgb;
    }
    [branch]
    if (bloomIntensity > 0.0f)
    {
        hdr += texBloom.Sample(sampLinearClamp, uv).rgb * bloomIntensity;
    }
    float ndcDepth = 1.0f;
    [branch]
    if (dofBlurRadius > 0.0f || fogDensity > 0.0f || underwaterStrength > 0.0f)
        ndcDepth = texDepth.Sample(sampLinearClamp, uv).r;
    hdr = ApplyDepthOfFieldHDR(hdr, uv, ndcDepth);
    hdr = ApplySharpenHDR(hdr, uv);

    // ---- Advanced Graphics: SSR 反射をトーンマップ前 (HDR 空間) でブレンドする ----
    // WHY: HDR 空間でブレンドすることで金属の映り込みが過露出部分でも正しく飽和する。
    //      ssrIntensity=0 のときはテクスチャが未束縛でも 0 を返すため分岐不要。
    if (ssrIntensity > 0.0f)
    {
        float4 ssrSample = texSSR.Sample(sampLinearClamp, uv);
        // alpha は Fresnel・roughness を含む信頼度。強度はここで一度だけ適用する。
        hdr = lerp(hdr, ssrSample.rgb, saturate(ssrSample.a * ssrIntensity));
    }
    // ---- Volumetric Light を HDR に加算合成する ----
    // WHY: 加算なので暗い領域に光の筋が自然に乗り、tonemapper がクランプする。
    if (volLightIntensity > 0.0f)
    {
        float3 volSample = texVolumetric.Sample(sampLinearClamp, uv).rgb;
        // Compute は物理量だけを書き、ユーザー強度はこのパスで一度だけ適用する。
        hdr += volSample * volLightIntensity;
    }

    // フロクセル霧は HDR のまま、トーンマップより前に乗せる。
    // WHY: 霧は光そのもので、露出とトーンマップを一緒に受けるべきもの。
    //      LDR 側で足すと、明るいシーンで霧だけが白飛びして浮く。
    hdr = ApplyFroxelFog(hdr, uv, ndcDepth);

    // 露出 → ACES トーンマップ → sRGB ガンマ補正
    float3 ldr = FinalOutput(hdr, ResolveExposure());
    ldr = ApplyClarity(ldr, uv);

    // 深度から線形距離を復元してフォグを適用する。
    // ndcZ ≥ 0.9999 はスカイドーム（clip.xyww で z=w → NDC z=1.0）なので霧を掛けない。
    // フォグの適用ロジック (ApplyFog) は共通で、色の出どころだけ fogSource で切り替える (§3-3)。
    if (fogDensity > 0.0f)
    {
        if (ndcDepth < 0.9999f)
        {
            float linDepth = LinearDepthFromNdc(ndcDepth);
            float dist     = max(linDepth - fogFar, 0.0f);
            float factor   = FogFactor(dist, fogDensity);

            float3 col = fogColor; // Exponential: 固定フォグ色
            if (fogSource > 0.5f)
            {
                // Atmosphere (エアリアルパースペクティブ): 視線方向の大気 in-scatter をフォグ色に使う。
                // 太陽から離れた遠景は青く、太陽方向は暖色に霞む。空ドームの見た目と整合する。
                float3 worldPos  = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
                float3 rayDir    = normalize(worldPos - cameraPos);
                float3 sunDir    = normalize(-lightDir);
                // skyDimmer を使う (Skydome / SunMoon と同じ軸)。lightIntensity を使うと
                // DirectionalLight を強めただけでエアリアルパースが空ドームと食い違う。
                float  scaled    = sunIntensity * skyDimmer;
                float3 inscatter = ComputeAtmosphericScattering(
                    rayDir, sunDir, rayleighScattering, mieScattering, mieG, scaled) * lightColor;
                // フォグは LDR 空間で適用するため、in-scatter (HDR) を露出→トーンマップして合わせる。
                col = FinalOutput(inscatter, ResolveExposure());
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
        if (ndcDepth < 0.9999f)
        {
            float linDepth = LinearDepthFromNdc(ndcDepth);
            depthFactor = saturate((1.0f - exp(-underwaterFogDensity * linDepth)) * underwaterStrength);
        }

        float wave = sin((uv.x + uv.y) * 28.0f + time * 2.4f) * 0.003f * underwaterStrength;
        float2 distortedUV = saturate(uv + float2(wave, wave * 0.5f));
        float3 distortedHdr = texHDR.Sample(sampLinearClamp, distortedUV).rgb;
        float3 distortedLdr = FinalOutput(distortedHdr, ResolveExposure());

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

    // ユーザー設定の明るさはフェードより先に掛ける。
    // WHY 順序が要るか: 逆にするとフェードアウトの黒が明るさで持ち上がり、暗転しきらない。
    ldr *= max(userBrightness, 0.0f);

    // 画面フェード — 全エフェクト適用後の最終合成として上書きする。
    // WHY: UI・ポストプロセス含む全レイヤーをひとつの lerp でカバーし、シーン遷移時のフラッシュを防ぐ。
    if (screenFadeAlpha > 0.0f)
        ldr = lerp(ldr, screenFadeColor, saturate(screenFadeAlpha));

    return float4(ldr, 1.0f);
}
