// FBZZ Engine
// Pipeline/Deferred/DepthCopy.hlsl | Pipeline
// GBuffer の深度テクスチャを hdrRT の深度バッファへ転写する
// フルスクリーン三角形で実行し、SV_Depth 出力で深度値を上書きする。
// Deferred パスで Sky / スキンドフォワードが正しい深度でテストできるようにするために必要。

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texGBufDepth, TEX_DEPTH_SLOT);  // t7: gbufferRT 深度 SRV

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

float PSMain(FSTriVSOut p) : SV_Depth
{
    // WHY: このパスの出力先は colorCount=0 の深度専用 RT。
    //      SV_Target を宣言すると D3D11 は RTV slot 0 を要求するため、深度だけを返す。
    return texGBufDepth.Load(int3((int)p.svPosition.x, (int)p.svPosition.y, 0)).r;
}
