// FBZZ Engine
// VolumetricLight.cs.hlsl | PostProcess/Lighting
// Volumetric Lighting — Compute Shader (DX11 SM5.0)
//
// 概要:
//   カメラ → シーン表面までのレイをレイマーチし、各ステップで
//   シャドウマップをサンプルして「光が通過しているか」を判定する。
//   通過している点では Henyey-Greenstein フェーズ関数で散乱光量を積分する。
//
// バインディング:
//   t7  = texDepth        (シーン深度)
//   t8  = texShadow       (シャドウマップ)
//   s0  = sampDefault     (通常サンプラー)
//   s1  = sampShadow      (比較サンプラー)
//   u4  = OutputVolumetric
//   b0  = CameraConstants
//   b3  = LightConstants
//   b4  = ShadowConstants
//   b8  = AdvancedGraphicsConstants
//
// スレッドグループ: [8, 8, 1]

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Rendering/Shadow.hlsli"

// ---- リソース ---------------------------------------------------------------

Texture2D<float>         texDepth  : register(t7);
Texture2D<float>         texShadow : register(t8);

SamplerState             sampDefault : register(s0);
SamplerComparisonState   sampShadow  : register(s1);

// 出力 (UAV_VOLUMETRIC = u4)
RWTexture2D<float4>      OutputVolumetric : register(u4);

// ---- フェーズ関数 -----------------------------------------------------------

// Henyey-Greenstein フェーズ関数
// cosTheta : 入射方向と散乱方向のコサイン
// g        : 非対称パラメータ (-1=後方散乱, 0=等方, 1=前方散乱)
// 戻り値    : 正規化された散乱確率密度
// WHY: HG 関数は計算コストが低く、大気・霧・雲など広範な散乱現象を近似できる
float HG(float cosTheta, float g)
{
    float g2  = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    // 4π で正規化された形 (PDF)
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(denom, 1e-6f), 1.5f));
}

// ---- メインカーネル ---------------------------------------------------------

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float2 outputSize;
    OutputVolumetric.GetDimensions(outputSize.x, outputSize.y);

    if ((float)id.x >= outputSize.x || (float)id.y >= outputSize.y)
        return;

    float2 uv = (float2(id.xy) + 0.5f) / outputSize;

    // ---- 深度からワールド座標を復元 ------------------------------------------

    float ndcDepth = texDepth.SampleLevel(sampDefault, uv, 0).r;

    // スカイボックス (depth=1) はレイマーチ距離を最大距離で制限
    float3 worldPos;
    if (ndcDepth >= 1.0f)
    {
        // 最大距離方向のワールド座標を算出
        float4 ndcFar = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, 1.0f, 1.0f);
        float4 wFar   = mul(ndcFar, invViewProjection);
        float3 dirW   = normalize(wFar.xyz / wFar.w - cameraPos);
        worldPos      = cameraPos + dirW * volMaxDist;
    }
    else
    {
        worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
        // 最大距離で制限: 遠くのサーフェスまで無制限にマーチすると重くなるため
        float dist = length(worldPos - cameraPos);
        if (dist > volMaxDist)
        {
            worldPos = cameraPos + normalize(worldPos - cameraPos) * volMaxDist;
        }
    }

    // ---- レイマーチ ----------------------------------------------------------

    if (volSteps <= 0 || volMaxDist <= 0.0f || volLightIntensity <= 0.0f)
    {
        OutputVolumetric[id.xy] = 0.0f;
        return;
    }

    float3 rayStart  = cameraPos;
    float3 rayEnd    = worldPos;
    float3 rayDir    = rayEnd - rayStart;
    float  rayLength = length(rayDir);
    rayDir           = rayDir / max(rayLength, 1e-6f); // 正規化

    float stepDist   = rayLength / float(volSteps);

    // カメラ → ライト方向のコサイン (散乱方向)
    float3 L         = normalize(-lightDir); // lightDir はライト → ワールドの向き
    float  cosTheta  = dot(rayDir, L);

    // Henyey-Greenstein 散乱値 (レイ方向依存、ループ外で計算)
    // WHY: cosTheta はステップごとに変わらないので外で一度計算してコストを下げる
    float scatter    = HG(cosTheta, volScattering);

    float3 accumulated = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (int step = 0; step < volSteps; ++step)
    {
        // ステップ中点でサンプル (ハーフステップオフセットで端点のアーティファクトを低減)
        float3 samplePos = rayStart + rayDir * (float(step) + 0.5f) * stepDist;

        // シャドウマップで「このサンプル点が照らされているか」を判定
        // ComputeShadow は ShadowConstants(b4) の shadowPcfRadius を内部で使用する
        float visibility = ComputeShadow(
            texShadow, sampShadow,
            samplePos,
            lightViewProjection,
            shadowMapTexelSize,
            shadowBias,
            float3(0.0f, 1.0f, 0.0f), // 体積光は法線なし — バイアスは固定値で十分
            L
        );

        // 照らされている点のみ散乱光を積分
        // 単位: lightColor * skyDimmer * scatter * stepDist (ビールランベルト近似)
        // WHY skyDimmer: 光芒は空ドーム・雲と一体で見える大気表現なので、地表ライティング用の
        //      lightIntensity ではなく空側の軸に乗せる。太陽を強くしたときに光芒だけが
        //      飽和するのを避け、明るさは volLightIntensity で独立に詰められる。
        accumulated += lightColor * skyDimmer * scatter * visibility * stepDist;
    }

    // ---- 出力 ----------------------------------------------------------------

    // volLightIntensity で全体スケールを調整
    float3 result = accumulated * volLightIntensity;
    OutputVolumetric[id.xy] = float4(result, 1.0f);
}
