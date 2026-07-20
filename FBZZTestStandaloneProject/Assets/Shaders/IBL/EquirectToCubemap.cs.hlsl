// FBZZ Engine
// IBL/EquirectToCubemap.cs.hlsl | IBL Baking
// Equirectangular HDR 画像を 6 面 Cubemap (Texture2DArray) に変換する Compute Shader
//
// 設計:
//   - 1 Dispatch = 1 面。Editor 側が 6 回ループして全面を焼き付ける。
//   - 各スレッドが Cubemap の 1 テクセルを担当し、方向ベクトルから Equirect UV を逆算する。
//   - 入力は R32G32B32A32_FLOAT (stb_image / TinyEXR の float* から CPU アップロード)
//   - 出力は R16G16B16A16_FLOAT の Texture2DArray UAV (1 面分)
//
// ディスパッチ: Dispatch(ceil(size/8), ceil(size/8), 1) を 6 回
//
// CB b0 — CbIblFace
//   faceIndex  : 処理対象の面 (0〜5)
//   textureSize: 出力 Cubemap の 1 辺のサイズ (px)

#include "IBL/IBLCommon.hlsli"

cbuffer CbIblFace : register(b0)
{
    uint g_faceIndex;
    uint g_textureSize;
    uint _pad0;
    uint _pad1;
};

// t0: Equirectangular HDR テクスチャ (R32G32B32A32_FLOAT, 2D)
Texture2D<float4>            g_equirect : register(t0);
// u0: 出力 Cubemap (R16G16B16A16_FLOAT, Texture2DArray — face ごとに 1 スライス分の UAV)
RWTexture2DArray<float4>     g_output   : register(u0);
// s0: LinearWrap (Equirect サンプリング用)
SamplerState                 g_sampler  : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_textureSize || dtid.y >= g_textureSize) return;

    // テクセル中心を [0, 1] UV へ
    float2 uv  = (dtid.xy + 0.5f) / float(g_textureSize);

    // Cubemap テクセル → ワールド方向ベクトル
    float3 dir = CubeTexelToDirection(g_faceIndex, uv);

    // ワールド方向 → Equirect UV
    float2 eqUV = DirToEquirectUV(dir);

    // Equirect テクスチャからサンプリング (mip 0 固定)
    float4 color = g_equirect.SampleLevel(g_sampler, eqUV, 0.0f);

    // 出力 (UAV スライスインデックスは呼び出し側が face ごとに設定する)
    g_output[uint3(dtid.x, dtid.y, 0)] = color;
}
