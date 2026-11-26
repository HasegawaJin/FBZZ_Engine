/// @file    DebugDraw.hlsl
/// @brief   デバッグ描画用シェーダー。頂点カラーで色を指定する (Line / Box / Sphere / Capsule)
/// @author  Hasegawa Jin
/// @date    2026-05-17

#include "Common/Binding.hlsli"
cbuffer CameraConstants : register(CB_CAMERA) {
    float4x4 viewProjection;
};

struct VSInput {
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), viewProjection);
    output.color    = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return input.color;
}
