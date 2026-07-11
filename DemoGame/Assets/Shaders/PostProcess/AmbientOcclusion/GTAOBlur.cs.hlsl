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
#include "Platform/DX11.hlsli"

Texture2D<float> texGTAORaw  : register(TEX_GTAO);      // GTAO RAW 入力
SamplerState     sampDefault : register(SAMPLER_DEFAULT);

RWTexture2D<float> OutputGTAO : register(UAV_GTAO_BLUR); // ブラー後出力

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;

    // 画面範囲外のスレッドは早期リターン
    if (pixel.x >= (uint)screenSize.x || pixel.y >= (uint)screenSize.y)
        return;

    float2 uv = (float2(pixel) + 0.5f) * texelSize;

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
            float2 offset = float2(x, y) * texelSize;
            result += texGTAORaw.SampleLevel(sampDefault, uv + offset, 0);
        }
    }

    OutputGTAO[pixel] = result / 16.0f;
}
