// FBZZ Engine
// PostProcess/Motion/MotionBlur.cs.hlsl | PostProcess
// Camera Motion Blur — 前フレームとの再投影差分をモーションベクトルとして使い、
// ベクトル方向にサンプルを積算してカメラ移動由来のブラーを再現する
//
// アルゴリズム概要:
//   1. 現フレームの深度からワールド座標を復元
//   2. prevViewProjection でそのワールド座標を前フレームクリップ空間に投影
//   3. モーションベクトル = 現 UV - 前フレーム UV
//   4. motionBlurStrength でスケールし、motionBlurSamples 点をサンプルして平均
//
// 入力バインディング:
//   t5  = 入力カラー    (TEX_GBUFFER0)
//   t7  = 深度バッファ  (TEX_DEPTH)
//   u5  = 出力 UAV      (UAV_MOTION_BLUR)
//   b0  = CameraConstants
//   b8  = AdvancedGraphicsConstants (screenWidth/screenHeight を含む)
//
// WHY b5 (PostProcConstants) を使わない:
//   MotionBlur パスは CompositePass より前に実行されるため b5 が未更新。
//   代わりに b8 の screenWidth/screenHeight を参照する。
//
// Dispatch サイズ: ceil(width/8) x ceil(height/8) x 1

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"

Texture2D        texColor : register(TEX_GBUFFER0); // 入力カラー（LDR/HDR）
Texture2D<float> texDepth : register(TEX_DEPTH);    // 深度バッファ

SamplerState sampDefault : register(SAMPLER_DEFAULT);

RWTexture2D<float4> OutputBlur : register(UAV_MOTION_BLUR); // モーションブラー出力

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 pixel = id.xy;

    // 画面範囲外のスレッドは早期リターン
    // WHY: b8 の screenWidth/screenHeight を使用 (b5 は本パスの実行時点で未バインド)
    if (pixel.x >= (uint)screenWidth || pixel.y >= (uint)screenHeight)
        return;

    float2 mbTexelSize = float2(1.0f / screenWidth, 1.0f / screenHeight);
    float2 uv = (float2(pixel) + 0.5f) * mbTexelSize;

    // ── 深度からワールド座標を復元 ──────────────────────────────────────────────
    float  ndcZ     = texDepth.SampleLevel(sampDefault, uv, 0).r;
    float3 worldPos = ReconstructWorldPos(uv, ndcZ, invViewProjection);

    // ── 前フレームの UV を計算（再投影）────────────────────────────────────────
    float4 prevClip = mul(float4(worldPos, 1.0f), prevViewProjection);
    prevClip.xyz   /= prevClip.w;
    float2 prevUV   = NdcToUv(prevClip.xy);

    // ── モーションベクトル（現 UV → 前フレーム UV の差分）────────────────────────
    // WHAT: カメラが動いた分だけ UV がずれるので、その差分がブラー方向になる
    float2 motionVec = (uv - prevUV) * motionBlurStrength;

    // ── モーションベクトルが極小の場合はブラーなし ─────────────────────────────
    float motionLen = length(motionVec);
    if (motionLen < 1e-5f || motionBlurSamples <= 0)
    {
        OutputBlur[pixel] = texColor.SampleLevel(sampDefault, uv, 0);
        return;
    }

    // ── motionBlurSamples 点を積算してブラーを適用 ─────────────────────────────
    // WHY: サンプル数をユーザーが可変にすることで品質とパフォーマンスのトレードオフを制御できる
    float4 accumulated = float4(0.0f, 0.0f, 0.0f, 0.0f);
    int    samples     = clamp(motionBlurSamples, 1, 32);

    for (int i = 0; i < samples; ++i)
    {
        // 現 UV からブラー方向へ均等に配置したサンプル点
        // t = [-0.5, 0.5] の範囲で現フレームを中心にブラーをかける
        // WHY: samples == 1 のとき (samples - 1) = 0 で ÷0 になるため max でガード。
        float  t         = (samples > 1) ? ((float(i) / float(samples - 1)) - 0.5f) : 0.0f;
        float2 sampleUV  = saturate(uv + motionVec * t);
        accumulated += texColor.SampleLevel(sampDefault, sampleUV, 0);
    }

    OutputBlur[pixel] = accumulated / float(samples);
}
