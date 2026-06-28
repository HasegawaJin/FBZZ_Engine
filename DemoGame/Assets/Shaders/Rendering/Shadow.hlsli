// FBZZ Engine
// Shadow.hlsli | Rendering
// PCF シャドウサンプリング
#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

#include "Common/Space.hlsli"

// AdvancedGraphicsConstants(b8) を宣言していないシェーダー向けフォールバック。
// WHY: HLSL コンパイラはエントリポイントから到達できない関数でも全ボディを検証するため、
//      pcssEnabled / pcssLightRadius が未定義だとコンパイルエラーになる。
//      Terrain や Water 等 b8 を持たないシェーダーでは static const でデフォルト値を提供し、
//      PCSS を PCF にフォールバックさせる（Constants.hlsli が define を立てたシェーダーは本ブロックを飛ばす）。
#ifndef HAVE_ADVANCED_GRAPHICS_CB
static const int   pcssEnabled     = 0;    // PCF にフォールバック
static const float pcssLightRadius = 1.0f; // 未使用（pcssEnabled=0 のため）
#endif

// =========================================================================
// PCF (Percentage Closer Filtering) シャドウ
//   shadowMap    : Texture2D<float> — シャドウデプスバッファ
//   shadowSampler: SamplerComparisonState (GREATER_EQUAL / BORDER=1.0)
//   uv           : シャドウマップ UV [0,1]
//   depth        : ライト空間の深度値 - バイアス (比較基準)
//   texelSize    : 1.0 / シャドウマップ解像度
//   radius       : PCF カーネル半径 (1 = 3x3, 2 = 5x5)
//   戻り値        : 0.0=完全に影, 1.0=完全に照らされている
//
//   WHY GREATER_EQUAL:
//     SampleCmpLevelZero は "stored COMP compare" を評価する。
//     照らされているピクセル: stored ≈ receiver_depth >= receiver-bias → 1.0(lit) ✓
//     影のピクセル: stored = blocker_depth < receiver_depth → 0.0(shadow) ✓
// =========================================================================
float SampleShadowPCF(Texture2D<float> shadowMap,
                      SamplerComparisonState shadowSampler,
                      float2 uv, float depth, float2 texelSize, int radius)
{
    float shadow = 0.0f;
    float total  = 0.0f;

    for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
    {
        float2 offset = float2(x, y) * texelSize;
        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, uv + offset, depth);
        total  += 1.0f;
    }

    return shadow / total;
}

// =========================================================================
// ComputeShadow — ワールド座標からシャドウ係数を計算する
//   N, L を受け取りスロープスケールバイアスを適用して Self-Shadow アクネを防ぐ。
//   UV が [0,1] 外 (ライト錐台外) は常に 1.0 (照らされている) を返す。
// =========================================================================
float ComputeShadow(Texture2D<float> shadowMap,
                    SamplerComparisonState shadowSampler,
                    float3 worldPos, float4x4 lightVP,
                    float2 texelSize, float bias,
                    float3 N, float3 L)
{
    float2 uv;
    float  depth;
    WorldToShadowUV(worldPos, lightVP, uv, depth);

    // ライト錐台の外は影なし
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;

    // スロープスケールバイアス: 斜め面で tan(theta) に比例してバイアスを増やす
    float NdotL        = saturate(dot(N, L));
    float slope        = sqrt(1.0f - NdotL * NdotL) / max(NdotL, 1e-4f);
    float adjustedBias = clamp(bias + bias * slope, bias, bias * 6.0f);

    // shadowPcfRadius は ShadowConstants cbuffer から参照。全シェーダー共通で Inspector から制御可能。
    float factor = SampleShadowPCF(shadowMap, shadowSampler, uv, depth - adjustedBias, texelSize, shadowPcfRadius);
    // shadowStrength: 1=完全な影, 0=影なし。factor=0(影) の時に (1-strength) を最小値とする。
    return lerp(1.0f - shadowStrength, 1.0f, factor);
}


// ============================================================
// PCSS (Percentage Closer Soft Shadows)
// ============================================================
// ComputeShadowPCSS — PCSS アルゴリズムによるソフトシャドウ計算。
// pcssEnabled == 0 の場合は通常 PCF にフォールバックする。
//
// アルゴリズム:
//   1. Blocker Search: searchRadius 範囲のシャドウマップを Poisson ディスクで
//      サンプルし、遮蔽ブロッカーの平均深度 avgBlockerDepth を算出する。
//   2. Penumbra Size: 受光点の深度と平均ブロッカー深度の差から半影幅を推定する。
//      penumbraWidth = (receiverDepth - avgBlockerDepth) * pcssLightRadius / avgBlockerDepth
//   3. PCF: penumbraWidth に比例したカーネルサイズで SampleShadowPCF を呼ぶ。

// 16点 Poisson ディスクサンプル (正規化済み [-1, 1])
// WHY: ランダムサンプルより均一分布で品質安定、DX11 SM5.0 でもコンパイル可能なハードコード
static const float2 PCSS_POISSON_DISK[16] =
{
    float2(-0.9444691f, -0.1409346f),
    float2(-0.8174540f,  0.4730292f),
    float2(-0.5808706f, -0.7196234f),
    float2(-0.5117652f,  0.1507150f),
    float2(-0.2847428f, -0.3699391f),
    float2(-0.2332620f,  0.6887614f),
    float2(-0.0374701f,  0.1408028f),
    float2( 0.0491168f, -0.7641724f),
    float2( 0.1052420f, -0.1734053f),
    float2( 0.2195470f,  0.5095905f),
    float2( 0.3937028f, -0.5105020f),
    float2( 0.4552360f,  0.1927327f),
    float2( 0.5760750f, -0.0651610f),
    float2( 0.6477049f,  0.6349671f),
    float2( 0.8018500f, -0.3605010f),
    float2( 0.9350649f,  0.2291007f),
};

// =========================================================================
// FindBlockerDepth — PCSS ステップ 1: ブロッカー探索
//   shadowMap     : シャドウデプスバッファ (SampleLevel 用)
//   pointSampler  : 通常 SamplerState (比較なし)
//   uv            : シャドウマップ UV
//   receiverDepth : 受光点の深度値
//   texelSize     : 1.0 / シャドウマップ解像度
//   searchRadius  : 探索半径 (テクセル単位)
//   戻り値         : 遮蔽ブロッカーの平均深度、見つからない場合は -1.0
// =========================================================================
float FindBlockerDepth(Texture2D<float> shadowMap,
                       SamplerState     pointSampler,
                       float2           uv,
                       float            receiverDepth,
                       float2           texelSize,
                       float            searchRadius)
{
    float blockerSum   = 0.0f;
    int   blockerCount = 0;

    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        float2 offset       = PCSS_POISSON_DISK[i] * searchRadius * texelSize;
        float  blockerDepth = shadowMap.SampleLevel(pointSampler, uv + offset, 0).r;

        // 受光点より手前にあるテクセルがブロッカー
        if (blockerDepth < receiverDepth)
        {
            blockerSum += blockerDepth;
            blockerCount++;
        }
    }

    return (blockerCount > 0) ? (blockerSum / float(blockerCount)) : -1.0f;
}

// =========================================================================
// ComputeShadowPCSS — ワールド座標から PCSS シャドウ係数を計算する
//   shadowMap     : Texture2D<float> シャドウデプスバッファ
//   shadowSampler : SamplerComparisonState (比較フィルタ用)
//   pointSampler  : SamplerState (ブロッカー探索用の通常サンプラー)
//   worldPos      : フラグメントのワールド座標
//   lightVP       : ライトのビュープロジェクション行列
//   texelSize     : 1.0 / シャドウマップ解像度
//   bias          : 深度オフセットバイアス
//   N             : 頂点法線 (ワールド空間、正規化済み)
//   L             : ライト方向 (ワールド空間、正規化済み)
//   戻り値         : 0.0=完全に影, 1.0=完全に照らされている
//
//   NOTE: pcssEnabled / pcssLightRadius / shadowStrength は
//         AdvancedGraphicsConstants(b8) / ShadowConstants(b4) から参照する。
//         呼び出し元シェーダーで両 cbuffer を宣言しておくこと。
// =========================================================================
float ComputeShadowPCSS(Texture2D<float>       shadowMap,
                        SamplerComparisonState shadowSampler,
                        SamplerState           pointSampler,
                        float3                 worldPos,
                        float4x4               lightVP,
                        float2                 texelSize,
                        float                  bias,
                        float3                 N,
                        float3                 L)
{
    // pcssEnabled == 0 なら通常 PCF にフォールバック (パフォーマンス優先モード)
    if (pcssEnabled == 0)
    {
        return ComputeShadow(shadowMap, shadowSampler, worldPos, lightVP, texelSize, bias, N, L);
    }

    // UV / 深度の取得
    float2 uv;
    float  receiverDepth;
    WorldToShadowUV(worldPos, lightVP, uv, receiverDepth);

    // ライト錐台外は照らされている
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;

    // スロープスケールバイアスで Self-Shadow アクネを防ぐ
    float NdotL        = saturate(dot(N, L));
    float slope        = sqrt(1.0f - NdotL * NdotL) / max(NdotL, 1e-4f);
    float adjustedBias = clamp(bias + bias * slope, bias, bias * 6.0f);
    receiverDepth     -= adjustedBias;

    // ---- ステップ 1: ブロッカー探索 ----
    // WHY: pcssLightRadius が大きいほど広い範囲でブロッカーを探し、より広い半影を生成する
    float searchRadius = pcssLightRadius * 10.0f; // world-space → テクセル空間の近似スケール
    float avgBlocker   = FindBlockerDepth(shadowMap, pointSampler,
                                          uv, receiverDepth, texelSize, searchRadius);

    // ブロッカーなし = 完全照射
    if (avgBlocker < 0.0f)
        return 1.0f;

    // ---- ステップ 2: 半影幅の推定 ----
    // 受光点の深度とブロッカー深度の差が大きいほど半影が広がる
    // 式: penumbraWidth = (d_receiver - d_blocker) * lightRadius / d_blocker
    float penumbraWidth = (receiverDepth - avgBlocker) * pcssLightRadius / max(avgBlocker, 1e-5f);

    // テクセル単位のカーネル半径を算出 (最小 1, 最大 8)
    // WHY: 上限 8 は SM5.0 でのループ展開コスト上限を考慮した実用値
    int pcfRadius = (int)clamp(penumbraWidth * 512.0f, 1.0f, 8.0f);

    // ---- ステップ 3: 可変カーネル PCF ----
    float factor = SampleShadowPCF(shadowMap, shadowSampler, uv, receiverDepth, texelSize, pcfRadius);

    return lerp(1.0f - shadowStrength, 1.0f, factor);
}

#endif // SHADOW_HLSLI
