/// @file    EquirectToCubemap.cs.hlsl
/// @brief   Equirectangular HDR 画像を 6 面 Cubemap (Texture2DArray) に変換する Compute Shader
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note 設計:
/// @note - 1 Dispatch = 1 面。Editor 側が 6 回ループして全面を焼き付ける。
/// @note - 各スレッドが Cubemap の 1 テクセルを担当し、方向ベクトルから Equirect UV を逆算する。
/// @note - 入力は R32G32B32A32_FLOAT (stb_image / TinyEXR の float* から CPU アップロード)
/// @note - 出力は R16G16B16A16_FLOAT の Texture2DArray UAV (1 面分)
///
/// @note ディスパッチ: Dispatch(ceil(size/8), ceil(size/8), 1) を 6 回
///
/// @note CB b0 — CbIblFace
/// @note faceIndex  : 処理対象の面 (0〜5)
/// @note textureSize: 出力 Cubemap の 1 辺のサイズ (px)

#include "IBL/IBLCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

cbuffer CbIblFace : register(FBZZ_REGISTER(b, CB_IBL_FACE_SLOT))
{
    uint g_faceIndex;
    uint g_textureSize;
    uint _pad0;
    uint _pad1;
};

/// @note t0: Equirectangular HDR テクスチャ (R32G32B32A32_FLOAT, 2D)
FBZZ_TEX2D_T(float4, g_equirect, SRV_IBL_SOURCE_SLOT);
/// @note u0: 出力 Cubemap (R16G16B16A16_FLOAT, Texture2DArray — face ごとに 1 スライス分の UAV)
FBZZ_RWTEX2DARRAY_T(float4, g_output, UAV_IBL_FACE_SLOT);
/// @note s0: LinearWrap (Equirect サンプリング用)
SamplerState                 g_sampler  : register(SAMPLER_DEFAULT);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_textureSize || dtid.y >= g_textureSize) return;

    /// @note テクセル中心を [0, 1] UV へ
    float2 uv  = (dtid.xy + 0.5f) / float(g_textureSize);

    /// @note Cubemap テクセル → ワールド方向ベクトル
    float3 dir = CubeTexelToDirection(g_faceIndex, uv);

    /// @note ワールド方向 → Equirect UV
    float2 eqUV = DirToEquirectUV(dir);

    /// @note Equirect テクスチャからサンプリング (mip 0 固定)
    float4 color = g_equirect.SampleLevel(g_sampler, eqUV, 0.0f);

    /// @note 出力 (UAV スライスインデックスは呼び出し側が face ごとに設定する)
    g_output[uint3(dtid.x, dtid.y, 0)] = color;
}
