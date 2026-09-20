/// @file BloomDownsample.cs.hlsl
/// @brief Bloom ミップ連鎖のダウンサンプル (1 段目だけ輝度閾値でフィルタする)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// Dispatch サイズ: ceil(dstWidth/8) x ceil(dstHeight/8) x 1
//
// texelSize    = 書き込み先のテクセルサイズ (UV を作るのに使う)
// bloomSrcTexel = 読み込み元のテクセルサイズ (タップのずらし幅に使う)

#include "Common/Constants.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSrc, TEX_BLOOM_SLOT);
// 全画面フェッチなので clamp 必須。s0 は DX12 の静的サンプラーが WRAP (メッシュの
// タイリング用) なので、ここで使うと画面端のカーネルが反対側の端を読み込む。
SamplerState       sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZ_RWTEX2D_T(float4, outputDst, UAV_OUTPUT_SLOT);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2 pixel = dtid.xy;
    uint2 outputSize;
    outputDst.GetDimensions(outputSize.x, outputSize.y);
    if (any(pixel >= outputSize))
        return;

    if (bloomIntensity <= 0.0f)
    {
        outputDst[pixel] = 0.0f;
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f) * texelSize;
    const float2 o  = bloomSrcTexel;

    // テント状の 5 タップ。
    // WHY 中心 1 タップで済ませないか: 書き込み先の画素中心は読み込み元の
    //     テクセル境界に乗るため、バイリニア 1 回で 2x2 の平均そのものになる。
    //     それだけだと畳み込みが箱型で、段を重ねると四角いにじみが残る。
    //     隣の 2x2 ブロックまで含めて 4x4 を滑らかに拾うと段間の継ぎ目が消える。
    float4 c = texSrc.SampleLevel(sampDefault, uv, 0) * 0.5f;
    c += texSrc.SampleLevel(sampDefault, uv + float2(-o.x, -o.y), 0) * 0.125f;
    c += texSrc.SampleLevel(sampDefault, uv + float2( o.x, -o.y), 0) * 0.125f;
    c += texSrc.SampleLevel(sampDefault, uv + float2(-o.x,  o.y), 0) * 0.125f;
    c += texSrc.SampleLevel(sampDefault, uv + float2( o.x,  o.y), 0) * 0.125f;

    // 輝度閾値は連鎖の 1 段目だけ。2 段目以降は既に選別済みのものをぼかすだけ。
    if (bloomApplyThreshold > 0.5f)
    {
        const float lum    = Luminance(c.rgb);
        const float knee   = max(bloomThreshold * bloomSoftKnee, 0.0001f);
        float       soft   = saturate((lum - bloomThreshold + knee) / (2.0f * knee));
        soft = soft * soft * knee;
        const float weight = max(lum - bloomThreshold, soft) / max(lum, 0.0001f);
        c.rgb *= weight;

        // NaN / Inf の混入を止める。HDR バッファは発散した値を持ちうるが、
        // ここで拾うとミップ全段へ広がって画面が丸ごと壊れる。
        /// @note min(NaN, x) は x を返すので、NaN は先に 0 へ落とす (放置すると最大輝度の光になる)。
        c.rgb = any(isnan(c.rgb)) ? float3(0.0f, 0.0f, 0.0f) : c.rgb;
        c.rgb = min(c.rgb, 65504.0f);
        c.rgb = max(c.rgb, 0.0f);
    }

    outputDst[pixel] = float4(c.rgb, c.a);
}
