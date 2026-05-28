// FBZZ Engine
// Pipeline/Deferred/DepthCopy.hlsl | Pipeline
// GBuffer の深度テクスチャを hdrRT の深度バッファへ転写する
// フルスクリーン三角形で実行し、SV_Depth 出力で深度値を上書きする。
// Deferred パスで Sky / スキンドフォワードが正しい深度でテストできるようにするために必要。

#include "Common/Binding.hlsli"

Texture2D texGBufDepth : register(TEX_DEPTH);  // t7: gbufferRT 深度 SRV

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

struct DepthCopyOut
{
    float color : SV_Target;
    float depth : SV_Depth;
};

DepthCopyOut PSMain(FSTriVSOut p)
{
    DepthCopyOut o;
    o.depth = texGBufDepth.Load(int3((int)p.svPosition.x, (int)p.svPosition.y, 0)).r;
    o.color = 0.0f;
    return o;
}
