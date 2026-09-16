// FBZZ Engine
// PostProcess/AmbientOcclusion/GTAOBlur.cs.hlsl | PostProcess
// GTAO ブラーパス — GTAO RAW テクスチャに 4x4 ボックスフィルターを適用してノイズを低減する
//
// SSAOBlur.cs.hlsl と同パターン。GTAO RAW は float1 テクスチャ (UAV_GTAO_RAW から出力)。
// WHY: GTAO はスライス数を抑えてパフォーマンスを稼ぐため、
//      ノイズが出やすい。ブラーパスで滑らかにしてから Composite に合成する。
//
// 入力バインディング:
//   t23 = GTAO RAW テクスチャ  (TEX_GTAO)
//   u7  = 出力 UAV             (UAV_GTAO_BLUR)
//   b5  = PostProcConstants    (texelSize, screenSize)
//
// Dispatch サイズ: ceil(width/8) x ceil(height/8) x 1

#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, texGTAORaw, TEX_GTAO_SLOT);      // GTAO RAW 入力
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState     sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZ_RWTEX2D_T(float, OutputGTAO, UAV_GTAO_BLUR_SLOT); // ブラー後出力

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;

    // 出力バッファ (半解像度対応) の実サイズを基準にする。ブラーは AO バッファ自身のテクセル幅で行う。
    // WHY: texelSize(b5) はフル解像度のため、半解像度バッファに使うとブラー半径が半分になり平滑化不足。
    float2 outSize;
    OutputGTAO.GetDimensions(outSize.x, outSize.y);
    if (pixel.x >= (uint)outSize.x || pixel.y >= (uint)outSize.y)
        return;

    float2 texel = 1.0f / outSize;
    float2 uv = (float2(pixel) + 0.5f) * texel;

    // ── 4x4 ボックスフィルター ───────────────────────────────────────────────────
    // WHY: SSAO ブラーと同じ [-2, 1] x [-2, 1] の 16 タップを使い、
    //      フィルタ特性を統一することで両 AO を同じ Composite パスに流しやすくする
    float result = 0.0f;

    [unroll]
    for (int y = -2; y <= 1; ++y)
    {
        [unroll]
        for (int x = -2; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            result += texGTAORaw.SampleLevel(sampDefault, uv + offset, 0);
        }
    }

    OutputGTAO[pixel] = result / 16.0f;
}
