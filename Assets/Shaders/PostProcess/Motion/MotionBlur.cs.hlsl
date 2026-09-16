// FBZZ Engine
// PostProcess/Motion/MotionBlur.cs.hlsl | PostProcess
// Camera Motion Blur — 前フレームとの再投影差分をモーションベクトルとして使い、
// ベクトル方向にサンプルを積算してカメラ移動由来のブラーを再現する
//
// アルゴリズム概要:
//   1. Velocity パスが書いたモーションベクターを引く (オブジェクトの動きを含む)
//   2. 書かれていない画素 (空・未描画) は深度 + prevViewProjection の再投影で補う
//   3. motionBlurStrength でスケールし、motionBlurSamples 点をサンプルして平均
//
// WHY 深度再投影だけでは足りないか:
//   再投影で得られるのはカメラの動きだけ。カメラを止めて撮ると、目の前を走る
//   キャラクターに一切ブラーがかからない。これは「カメラモーションブラー」であって
//   オブジェクトモーションブラーではない。
//
// 入力バインディング:
//   t5  = 入力カラー        (TEX_GBUFFER0)
//   t7  = 深度バッファ      (TEX_DEPTH)
//   t26 = モーションベクター (TEX_VELOCITY)
//   u5  = 出力 UAV          (UAV_MOTION_BLUR)
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
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texColor, TEX_GBUFFER0_SLOT); // 入力カラー（LDR/HDR）
FBZZ_TEX2D_T(float, texDepth, TEX_DEPTH_SLOT);    // 深度バッファ
FBZZ_TEX2D(texVelocity, TEX_VELOCITY_SLOT); // モーションベクター (RG=速度, B=有効)

// 全画面フェッチなので clamp 必須。速度方向へ uv を伸ばして引くので、
// s0 (DX12 では WRAP) だと画面端のブラーが反対側の端を巻き込む。
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZ_RWTEX2D_T(float4, OutputBlur, UAV_MOTION_BLUR_SLOT); // モーションブラー出力

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

    if (motionBlurStrength <= 0.0f || motionBlurSamples <= 0)
    {
        OutputBlur[pixel] = texColor.SampleLevel(sampDefault, uv, 0);
        return;
    }

    // ── 前フレームからの移動量を求める ──────────────────────────────────────────
    float2 frameMotion;
    float3 velocity = texVelocity.SampleLevel(sampDefault, uv, 0).rgb;
    if (velocity.z > 0.5f)
    {
        // Velocity パスが書いた画素。カメラとオブジェクトの動きが両方入っている。
        frameMotion = velocity.xy;
    }
    else
    {
        // 空や未描画の画素。カメラの動きしか無いので深度再投影で足りる。
        float  ndcZ     = texDepth.SampleLevel(sampDefault, uv, 0).r;
        float3 worldPos = ReconstructWorldPos(uv, ndcZ, invViewProjection);

        float4 prevClip = mul(float4(worldPos, 1.0f), prevViewProjection);
        prevClip.xyz   /= prevClip.w;
        frameMotion     = uv - NdcToUv(prevClip.xy);
    }

    float2 motionVec = frameMotion * motionBlurStrength;

    // ── モーションベクトルが極小の場合はブラーなし ─────────────────────────────
    float motionLen = length(motionVec);
    if (motionLen < 1e-5f)
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
