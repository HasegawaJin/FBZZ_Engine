/// @file    AOBlurCommon.hlsli
/// @brief   半解像度 AO の奥行き考慮ぼかし (SSAOBlur / GTAOBlur 共通)。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note    t7 = フル解像度のカメラ深度 (Reversed-Z)。b0 CameraConstants を使う。
/// @see     https://en.wikipedia.org/wiki/Bilateral_filter (Bilateral filter — 距離の重みに値の差の重みを掛ける)
#ifndef AO_BLUR_COMMON_HLSLI
#define AO_BLUR_COMMON_HLSLI

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, texAOBlurDepth, TEX_DEPTH_SLOT);

/// @brief 奥行きの差を中心の奥行きに対する比で測る幅。これを超えて離れた画素はほぼ寄与しない。
static const float kAOBlurDepthSharpness = 0.05f;
/// @brief 片側の半径 [半解像度の画素]。5×5 で中心に対して対称。
static const int kAOBlurRadius = 2;

/// @brief 半解像度の画素に対応するフル解像度の視空間 Z。空は farZ。
float AOBlurViewDepth(int2 halfPixel, float2 fullSize)
{
    const int2 texel = clamp(halfPixel * 2, int2(0, 0), int2(fullSize) - 1);
    return LinearizeDepth(texAOBlurDepth.Load(int3(texel, 0)), nearZ, farZ, isOrthographic);
}

/// @brief AO テクスチャの値から先頭の成分を取る。Texture2D<float> はスカラー、Texture2D<float4> はベクトルを返すため。
float AOFirstComponent(float value)  { return value; }
float AOFirstComponent(float4 value) { return value.x; }

/// @brief 半解像度の AO を奥行きで重み付けしてぼかす。
/// @param aoTex 半解像度の AO (先頭の成分を読む)。
/// @note 空間の重みは二項係数 (1,4,6,4,1) の縦横の積。奥行きの重みは exp(-|Δz| / (z * sharpness))。
///       箱型で奥行きを見ないと、手前の物体の輪郭を越えて背景の AO が混ざり縁に帯が出る。
template<typename TTexture>
float BlurAO(TTexture aoTex, int2 pixel, float2 outSize)
{
    float2 fullSize;
    texAOBlurDepth.GetDimensions(fullSize.x, fullSize.y);
    const float centerZ  = AOBlurViewDepth(pixel, fullSize);
    const float invWidth = 1.0f / max(centerZ * kAOBlurDepthSharpness, 1.0e-3f);
    const float binomial[5] = { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f };

    float sum = 0.0f;
    float weightSum = 0.0f;
    [unroll]
    for (int y = -kAOBlurRadius; y <= kAOBlurRadius; ++y)
    {
        [unroll]
        for (int x = -kAOBlurRadius; x <= kAOBlurRadius; ++x)
        {
            const int2  p  = clamp(pixel + int2(x, y), int2(0, 0), int2(outSize) - 1);
            const float dz = abs(AOBlurViewDepth(p, fullSize) - centerZ);
            const float w  = binomial[x + kAOBlurRadius] * binomial[y + kAOBlurRadius] * exp(-dz * invWidth);
            sum       += AOFirstComponent(aoTex.Load(int3(p, 0))) * w;
            weightSum += w;
        }
    }
    return sum / max(weightSum, 1.0e-5f);
}

#endif // AO_BLUR_COMMON_HLSLI
