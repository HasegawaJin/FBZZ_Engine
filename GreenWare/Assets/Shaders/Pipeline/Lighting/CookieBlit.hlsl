/// @file CookieBlit.hlsl
/// @brief ライト Cookie の元テクスチャをアトラスのタイル 1 枚へ焼き直す
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// ビューポートで切ったタイルへ全画面三角形を描くだけ。拡縮はサンプラーが行う。
//
// WHY アトラスへ焼き直すか: Cookie を持つライトは 1 フレームに複数あり、そのすべてが
//      1 回のピクセルシェーダー呼び出しの中で評価される。ライトごとに違うテクスチャを
//      バインドすることはできないので、1 枚へ集めて矩形で切り出すしかない。
//      焼き直しは Cookie の顔ぶれが変わったフレームだけ走る。

#include "Common/Fullscreen.hlsli"
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(gCookieSource, TEX_ALBEDO_SLOT);
SamplerState gCookieSamp   : register(SAMPLER_LINEAR_CLAMP);

cbuffer CookieBlitConstants : register(CB_MATERIAL)
{
    // 見かけの回転 [rad]。ライト自身を回すと影の向きまで変わるため、模様だけを回す軸。
    float cookieRotation;
    // 1 = 元テクスチャが sRGB エンコード。アトラスはリニアで持つ。
    // WHY リニアで持つか: サンプル結果はライト色へ乗算される。sRGB のまま掛けると
    //      中間調が実際より明るくなり、ゴボの濃淡がオーサリングした通りにならない。
    uint  cookieSrgb;
    float2 _cookiePad;
};

FBZZFullscreenVertex VSMain(uint vertexId : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(vertexId);
}

float4 PSMain(FBZZFullscreenVertex input) : SV_TARGET
{
    float2 uv = input.uv;

    if (abs(cookieRotation) > 1e-4f)
    {
        const float s = sin(cookieRotation);
        const float c = cos(cookieRotation);
        const float2 centered = uv - 0.5f;
        uv = float2(centered.x * c - centered.y * s,
                    centered.x * s + centered.y * c) + 0.5f;
    }

    // 回転で枠外へ出た部分は「遮り無し」= 白にする。
    // WHY 黒にしないか: Cookie は乗算マスクなので、外側を黒にすると回した瞬間に
    //     スポットの四隅が欠ける。素通りに倒す方が、回転が破壊的にならない。
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return float4(1.0f, 1.0f, 1.0f, 1.0f);

    float4 color = gCookieSource.SampleLevel(gCookieSamp, uv, 0);
    if (cookieSrgb != 0u)
        color.rgb = pow(saturate(color.rgb), 2.2f);
    return color;
}
