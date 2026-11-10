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
static const uint AO_BLUR_GROUP_SIZE = 8;
static const uint AO_BLUR_GROUP_THREADS = AO_BLUR_GROUP_SIZE * AO_BLUR_GROUP_SIZE;
static const uint AO_BLUR_TILE_SIZE = AO_BLUR_GROUP_SIZE + 2 * kAOBlurRadius;
static const uint AO_BLUR_TILE_COUNT = AO_BLUR_TILE_SIZE * AO_BLUR_TILE_SIZE;
groupshared float2 s_aoBlurTile[AO_BLUR_TILE_COUNT];

/// @brief 半解像度の画素に対応するフル解像度の視空間 Z。空は farZ。
float AOBlurViewDepth(int2 halfPixel, int2 fullSize)
{
    const int2 texel = clamp(halfPixel * 2, int2(0, 0), fullSize - 1);
    return LinearizeDepth(texAOBlurDepth.Load(int3(texel, 0)), nearZ, farZ, isOrthographic);
}

/// @brief AO テクスチャの値から先頭の成分を取る。Texture2D<float> はスカラー、Texture2D<float4> はベクトルを返すため。
float AOFirstComponent(float value)  { return value; }
float AOFirstComponent(float4 value) { return value.x; }

/// @note 8×8 の出力と周囲 2 画素を共有する。端の無効スレッドも同期まで参加する。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/groupmemorybarrierwithgroupsync GroupMemoryBarrierWithGroupSync
template<typename TTexture>
void LoadAOBlurTile(TTexture aoTex, uint2 groupId, uint groupIndex, uint2 outSize)
{
    uint2 fullSize;
    texAOBlurDepth.GetDimensions(fullSize.x, fullSize.y);
    const int2 origin = int2(groupId * AO_BLUR_GROUP_SIZE) - kAOBlurRadius;
    [unroll]
    for (uint baseIndex = 0; baseIndex < AO_BLUR_TILE_COUNT; baseIndex += AO_BLUR_GROUP_THREADS)
    {
        const uint index = groupIndex + baseIndex;
        if (index < AO_BLUR_TILE_COUNT)
        {
            const int2 offset = int2(index % AO_BLUR_TILE_SIZE, index / AO_BLUR_TILE_SIZE);
            const int2 p = clamp(origin + offset, int2(0, 0), int2(outSize) - 1);
            s_aoBlurTile[index] = float2(AOFirstComponent(aoTex.Load(int3(p, 0))),
                AOBlurViewDepth(p, int2(fullSize)));
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

/// @brief 半解像度の AO を奥行きで重み付けしてぼかす。
/// @note 空間の重みは二項係数 (1,4,6,4,1) の縦横の積。奥行きの重みは exp(-|Δz| / (z * sharpness))。
float BlurAO(uint2 localPixel)
{
    const int center = int((localPixel.y + kAOBlurRadius) * AO_BLUR_TILE_SIZE + localPixel.x + kAOBlurRadius);
    const float centerZ = s_aoBlurTile[center].y;
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
            const float2 sampleValue = s_aoBlurTile[center + y * int(AO_BLUR_TILE_SIZE) + x];
            const float dz = abs(sampleValue.y - centerZ);
            const float w  = binomial[x + kAOBlurRadius] * binomial[y + kAOBlurRadius] * exp(-dz * invWidth);
            sum       += sampleValue.x * w;
            weightSum += w;
        }
    }
    return sum / max(weightSum, 1.0e-5f);
}

#endif
