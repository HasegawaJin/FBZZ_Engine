/// @file    Downscale.hlsl
/// @brief   内部解像度が出力より大きいとき (renderScale > 1 のスーパーサンプリング)、出力 1 画素が覆う入力を箱型に平均して縮める。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note    b5 の screenSize / texelSize には «入力» の寸法が入る (UpscalePass と同じ契約)。
/// @note    双線形 1 タップで縮めると、倍率が 2 を超えたり整数でなかったりするとき入力の画素を読み飛ばし、
///          スーパーサンプリングで落としたはずのエイリアスが戻る。足跡を覆うようにタップを並べて平均する。
/// @see     https://en.wikipedia.org/wiki/Supersampling (Supersampling — 出力画素ごとの平均)

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSource, TEX_GBUFFER0_SLOT);
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

/// @brief 片軸のタップ数の上限。各タップは双線形なので 4 タップで 8 画素幅まで覆える。
static const int kMaxTapsPerAxis = 4;

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    /// @note 出力 1 画素が入力の何画素ぶんを覆うか。UV の画面微分から出すので出力の寸法は要らない。
    const float2 footprint = max(float2(abs(ddx(p.uv.x)), abs(ddy(p.uv.y))) * screenSize, 1.0f);
    /// @note 双線形タップは 2 画素を平均するので、タップ間隔を 2 画素以下にすれば足跡を取りこぼさない。
    const int2 taps = clamp(int2(ceil(footprint * 0.5f)), 1, kMaxTapsPerAxis);

    float3 sum = 0.0f;
    [loop]
    for (int y = 0; y < taps.y; ++y)
    {
        [loop]
        for (int x = 0; x < taps.x; ++x)
        {
            const float2 offset = ((float2(x, y) + 0.5f) / float2(taps) - 0.5f) * footprint * texelSize;
            sum += texSource.SampleLevel(sampLinear, p.uv + offset, 0).rgb;
        }
    }
    return float4(sum / float(taps.x * taps.y), 1.0f);
}
