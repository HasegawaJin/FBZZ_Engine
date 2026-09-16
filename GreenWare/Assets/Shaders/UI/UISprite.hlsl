// FBZZ Engine
// UISprite.hlsl | UI
// UIImage の既定シェーダー。テクスチャ 1 枚を色で染めて出すだけ。
//
// マテリアルを持たない UIImage はすべてこれで描かれる。パラメータを公開しないので
// MaterialConstants は宣言しない (宣言すると .mat 側に空の項目が並ぶだけになる)。
#include "UI/UICommon.hlsli"

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    return g_Texture.Sample(g_Sampler, input.uv) * UITint(input);
}
