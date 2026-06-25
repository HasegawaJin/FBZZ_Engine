// FBZZ Engine
// ContactShadows.cs.hlsl | PostProcess/Shadow
// Screen Space Contact Shadows — Compute Shader (DX11 SM5.0)
//
// 概要:
//   スクリーン空間で短距離レイマーチを行い、ライト方向に隣接オブジェクトが
//   遮蔽しているかどうかを判定する。シャドウマップでは解像度の制限から
//   拾えない "コンタクト部分の細かい影" を補完する。
//
//   出力: 1.0=照らされている / 0.0=遮蔽（影）
//   後段の DeferredLighting で既存シャドウ係数に乗算して合成する。
//
// バインディング:
//   t7  = texDepth     (シーン深度)
//   s0  = sampDefault  (通常サンプラー)
//   u3  = OutputContactShadow (UAV_CONTACT_SHADOW)
//   b0  = CameraConstants
//   b3  = LightConstants
//   b8  = AdvancedGraphicsConstants
//
// スレッドグループ: [8, 8, 1]

#include "Common/Binding.hlsli"
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"

// ---- リソース ---------------------------------------------------------------

Texture2D<float>   texDepth          : register(t7);
SamplerState       sampDefault       : register(s0);

// コンタクトシャドウマスク出力 (UAV_CONTACT_SHADOW = u3)
// 0.0 = 遮蔽（影）, 1.0 = 照らされている
RWTexture2D<float> OutputContactShadow : register(UAV_CONTACT_SHADOW);

// ---- メインカーネル ---------------------------------------------------------

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float2 outputSize;
    OutputContactShadow.GetDimensions(outputSize.x, outputSize.y);

    if ((float)id.x >= outputSize.x || (float)id.y >= outputSize.y)
        return;

    float2 uv = (float2(id.xy) + 0.5f) / outputSize;

    // ---- 深度 → ビュー空間位置 ----------------------------------------------

    float ndcDepth = texDepth.SampleLevel(sampDefault, uv, 0).r;

    // スカイボックスはコンタクトシャドウ不要
    if (ndcDepth >= 1.0f)
    {
        OutputContactShadow[id.xy] = 1.0f;
        return;
    }

    // NDC → ビュー空間座標を復元
    // WHY: ビュー空間でレイマーチすると NDC 投影が線形な除算で済み
    //      深度比較も同じビュー空間で統一できる
    float4 ndcPos    = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, ndcDepth, 1.0f);
    float4 viewPos4  = mul(ndcPos, transpose(projection)); // inv(projection) 相当
    // ホモジニアス除算
    float3 viewPos   = viewPos4.xyz / viewPos4.w;

    // ---- ライト方向をビュー空間に変換 ----------------------------------------

    // lightDir はワールド空間でのライト→サーフェス方向 (向き)
    // レイはサーフェス → ライト方向に飛ばすので反転する
    float3 lightDirWorld = normalize(-lightDir);
    float3 lightDirView  = normalize(mul(float4(lightDirWorld, 0.0f), view).xyz);

    // ---- スクリーン空間レイマーチ --------------------------------------------

    float stepLength = contactShadowRayLen / float(contactShadowSteps);

    // 現在の線形ビュー深度 (正値: DX11 ビュー空間は Z 負のため符号反転)
    float originDepth = -viewPos.z;

    float factor = 1.0f; // デフォルト: 遮蔽なし

    [loop]
    for (int step = 1; step <= contactShadowSteps; ++step)
    {
        // レイをビュー空間で進める
        float3 rayPosVS = viewPos + lightDirView * (stepLength * float(step));

        // ビュー空間 → クリップ空間に投影
        float4 clip = mul(float4(rayPosVS, 1.0f), projection);
        clip.xyz   /= clip.w;

        // クリップ空間が有効範囲外ならスキップ
        if (clip.x < -1.0f || clip.x > 1.0f ||
            clip.y < -1.0f || clip.y > 1.0f ||
            clip.z <  0.0f)
            continue;

        // クリップ → UV
        float2 rayUV = NdcToUv(clip.xy);

        // その UV の深度をサンプル
        float sampleNdcDepth  = texDepth.SampleLevel(sampDefault, rayUV, 0).r;
        float sampleDepthLin  = LinearizeDepth(sampleNdcDepth, nearZ, farZ);

        // レイの現在深度 (正値)
        float rayDepthLin     = -rayPosVS.z;

        // 深度テスト:
        //   abs(sampleDepth - rayDepth) < contactShadowThick なら遮蔽と判定
        // WHY: 単純に sampleDepth < rayDepth だけでは薄いサーフェスを貫通した
        //      レイが誤検知するため、厚み判定で False Positive を抑制する
        float depthDiff = abs(sampleDepthLin - rayDepthLin);
        if (sampleDepthLin < rayDepthLin && depthDiff < contactShadowThick)
        {
            factor = 0.0f; // 遮蔽
            break;
        }
    }

    // ---- contactShadowStrength を反映して出力 --------------------------------
    // strength=0 → 影の効果なし (factor そのまま 1.0)
    // strength=1 → 完全に factor を反映
    // WHY: 強度パラメータで遮蔽の濃さをデザイナーが調整できるようにする
    float result = lerp(1.0f, factor, contactShadowStrength);
    OutputContactShadow[id.xy] = result;
}
