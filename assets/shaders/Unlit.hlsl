// FBZZ Engine
// Unlit.hlsl | fbzz::renderer
// ライティングなし。頂点カラーまたはマテリアルカラーで単色描画する

cbuffer CameraConstants : register(b0) {
    float4x4 viewProjection;
    float3   cameraPos;
    float    _pad;
};

cbuffer ObjectConstants : register(b1) {
    float4x4 world;
};

cbuffer MaterialConstants : register(b2) {
    float4 color;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 worldPos  = mul(float4(input.position, 1.0f), world);
    output.position  = mul(worldPos, viewProjection);
    output.uv        = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return color;
}
