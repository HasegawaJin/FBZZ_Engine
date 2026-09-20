/// @file ExposureCommon.hlsli
/// @brief 自動露出 (眼の順応) の共通定義
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// LAYOUT: Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp の
//         AutoExposureCB と完全に一致させること。
#ifndef EXPOSURE_COMMON_HLSLI
#define EXPOSURE_COMMON_HLSLI

#include "Common/Binding.hlsli"

// ヒストグラムのビン数。1 グループ 256 スレッドで一気に畳めるので 256 に固定する。
#define FBZZ_EXPOSURE_BINS 256

cbuffer AutoExposureConstants : register(CB_MATERIAL)
{
    float exposureMinEV;        // ヒストグラムが覆う下端 [EV]
    float exposureEvRange;      // maxEV - minEV
    float exposureLowPercent;   // 捨てる暗部の割合 [0,1]
    float exposureHighPercent;  // 捨てる明部の割合 [0,1]

    float exposureSpeedUp;      // 明順応の速さ [1/秒]
    float exposureSpeedDown;    // 暗順応の速さ [1/秒]
    float exposureDeltaTime;    // 前フレームからの経過秒
    float exposureCompensation; // 手動オフセット [EV]

    float exposureMinEVClamp;   // 露出の下限 [EV]
    float exposureMaxEVClamp;   // 露出の上限 [EV]
    uint  exposureReset;        // 1 = 順応を飛ばして即座に合わせる (初回 / シーン切替)
    float _exposurePad0;
};

// 輝度 → ヒストグラムのビン番号。
// WHY 対数を取るか: 輝度は数桁にわたる。線形にビンを切ると、実用域である中間調が
//     数ビンに潰れ、超高輝度側に大半のビンを浪費する。
uint FBZZ_LuminanceToBin(float luminance)
{
    // 0 と極小値は 0 番へまとめる。log2(0) = -inf を踏まない。
    if (luminance < 1e-5f) return 0u;
    const float ev = log2(luminance);
    const float t  = saturate((ev - exposureMinEV) / max(exposureEvRange, 1e-4f));
    // 0 番は「実質黒」の専用枠にしてあるので、有効域は 1..255 へ写す。
    return (uint)clamp(t * float(FBZZ_EXPOSURE_BINS - 1) + 1.0f,
                       1.0f, float(FBZZ_EXPOSURE_BINS - 1));
}

// ビン番号 → そのビンが代表する輝度。
float FBZZ_BinToLuminance(uint bin)
{
    if (bin == 0u) return 0.0f;
    /// @note ビン k は t ∈ [(k-1), k) / (BINS-1) を受け持つので、代表値はその中央。
    const float t  = saturate((float(bin) - 0.5f) / float(FBZZ_EXPOSURE_BINS - 1));
    return exp2(t * exposureEvRange + exposureMinEV);
}

// Rec.709 の相対輝度。
float FBZZ_Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

#endif // EXPOSURE_COMMON_HLSLI
