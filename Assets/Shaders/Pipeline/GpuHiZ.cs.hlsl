/// @file    GpuHiZ.cs.hlsl
/// @brief   実描画深度から 4 段ずつ最遠深度の階層を作る。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, g_sourceDepth, 7);
FBZZ_RWSBUFFER_T(float, g_pyramid, UAV_CLUSTER_LIGHTS_SLOT);

cbuffer BuildConstants : register(b0)
{
    uint4 g_sourceRect;
    uint4 g_outputOffsets;
    uint g_sourceOffset;
    uint g_levelCount;
    uint g_readDepth;
    uint g_reversedZ;
};

static const uint BASE_TILE = 4;
static const uint THREAD_GROUP_SIDE = 8;
groupshared float g_groupDepth[THREAD_GROUP_SIDE * THREAD_GROUP_SIDE];

float Farthest(float a, float b)
{
    return g_reversedZ != 0 ? min(a, b) : max(a, b);
}

/// @note Clear の最遠値を保持し、端で範囲外になった要素だけを集約から外す。
float ReadTile(uint2 pixel)
{
    const float farDepth = g_reversedZ != 0 ? 0.0f : 1.0f;
    float depth = 1.0f - farDepth;
    if (g_readDepth != 0) {
        [unroll] for (uint y = 0; y < BASE_TILE; ++y) {
            [unroll] for (uint x = 0; x < BASE_TILE; ++x) {
                const uint2 source = pixel * BASE_TILE + uint2(x, y);
                if (all(source < g_sourceRect.zw)) {
                    float value = g_sourceDepth.Load(int3(g_sourceRect.xy + source, 0));
                    if (!isfinite(value) || value < 0.0f || value > 1.0f) value = farDepth;
                    depth = Farthest(depth, value);
                }
            }
        }
    } else {
        [unroll] for (uint y = 0; y < 2; ++y) {
            [unroll] for (uint x = 0; x < 2; ++x) {
                const uint2 source = pixel * 2 + uint2(x, y);
                if (all(source < g_sourceRect.zw))
                    depth = Farthest(depth, g_pyramid[g_sourceOffset + source.y * g_sourceRect.z + source.x]);
            }
        }
    }
    return depth;
}

/// @note 各 8×8 グループの範囲内で、互いに重ならない 2×2 を 3 回集約する。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/groupmemorybarrierwithgroupsync グループ共有メモリの可視化。
[numthreads(THREAD_GROUP_SIDE, THREAD_GROUP_SIDE, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const uint tile = g_readDepth != 0 ? BASE_TILE : 2;
    uint2 extent = (g_sourceRect.zw + tile - 1) / tile;
    const float depth = ReadTile(id.xy);
    g_groupDepth[lane] = depth;
    if (all(id.xy < extent)) g_pyramid[g_outputOffsets.x + id.y * extent.x + id.x] = depth;

    for (uint level = 1; level < g_levelCount; ++level) {
        GroupMemoryBarrierWithGroupSync();
        extent = (extent + 1) / 2;
        const uint side = 1u << level;
        const uint halfSide = side / 2;
        if (all((local.xy & (side - 1)) == 0)) {
            const uint row = halfSide * THREAD_GROUP_SIDE;
            const float farthest = Farthest(Farthest(g_groupDepth[lane], g_groupDepth[lane + halfSide]),
                Farthest(g_groupDepth[lane + row], g_groupDepth[lane + row + halfSide]));
            g_groupDepth[lane] = farthest;
            const uint2 output = id.xy >> level;
            if (all(output < extent))
                g_pyramid[g_outputOffsets[level] + output.y * extent.x + output.x] = farthest;
        }
    }
}
