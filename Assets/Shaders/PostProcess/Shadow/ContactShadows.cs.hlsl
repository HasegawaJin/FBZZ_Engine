/// @file    ContactShadows.cs.hlsl
/// @brief   スクリーン空間コンタクトシャドウ。シャドウマップの解像度では拾えない接地部の細かい影を補う。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note    出力は 1 = 照らされている / 0 = 遮蔽。後段のライティングが影係数へ乗算する。
/// @note    ビュー空間は左手系 (前方 +Z)。カメラ深度は Reversed-Z (near → 1、far → 0)。
/// @see     https://panoskarabelas.com/posts/screen_space_shadows/ (Panos Karabelas, "Screen space shadows")

#include "Common/Binding.hlsli"
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note t7 = シーン深度 (Reversed-Z)。u3 = 出力 (半解像度)。b0 Camera / b3 Light / b8 AdvancedGraphics。
FBZZ_TEX2D_T(float, texDepth, 7);
FBZZ_RWTEX2D_T(float, OutputContactShadow, UAV_CONTACT_SHADOW_SLOT);

/// @brief 自分自身の面を遮蔽物と取り違えないための奥行きの余裕 (視空間 Z に対する比)。
/// @note 深度バッファの量子化と、半解像度の出力画素が複数の深度画素を代表することによる誤差を吸う。
static const float kSelfShadowBias = 0.002f;

/// @brief UV の位置の深度を最近傍で読む。
/// @note 補間すると輪郭で手前と奥の深度が混ざり、存在しない面に当たって縁に影のにじみが出る。
float LoadDepth(float2 uv, float2 depthSize)
{
    const int2 pixel = clamp(int2(uv * depthSize), int2(0, 0), int2(depthSize) - 1);
    return texDepth.Load(int3(pixel, 0)).r;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float2 outputSize;
    OutputContactShadow.GetDimensions(outputSize.x, outputSize.y);
    if ((float)id.x >= outputSize.x || (float)id.y >= outputSize.y)
        return;

    float2 depthSize;
    texDepth.GetDimensions(depthSize.x, depthSize.y);

    const float2 uv = (float2(id.xy) + 0.5f) / outputSize;
    const float ndcDepth = LoadDepth(uv, depthSize);
    if (IsFarDepth(ndcDepth))
    {
        OutputContactShadow[id.xy] = 1.0f;
        return;
    }

    /// @note 視空間でレイを進める。投影は線形な除算で済み、奥行きの比較も視空間 Z で揃う。
    const float3 worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
    const float3 viewPos  = mul(float4(worldPos, 1.0f), view).xyz;

    /// @note lightDir はライト → 面の向き。レイは面 → ライトへ飛ばす。
    const float3 toLightView = normalize(mul(float4(normalize(-lightDir), 0.0f), view).xyz);

    const int   stepCount  = max(contactShadowSteps, 1);
    const float stepLength = contactShadowRayLen / float(stepCount);

    float factor = 1.0f;
    [loop]
    for (int step = 1; step <= stepCount; ++step)
    {
        const float3 rayPosVS = viewPos + toLightView * (stepLength * float(step));
        /// @note near 面より手前 (カメラの後ろを含む) へ出たら、その先は画面に映っていない。
        if (rayPosVS.z <= nearZ)
            break;

        float4 clip = mul(float4(rayPosVS, 1.0f), projection);
        if (clip.w <= 1.0e-5f)
            break;
        clip.xy /= clip.w;
        if (any(abs(clip.xy) > 1.0f))
            break;

        const float2 rayUV = NdcToUv(clip.xy);
        const float sceneDepth = LinearizeDepth(LoadDepth(rayUV, depthSize), nearZ, farZ, isOrthographic);
        const float rayDepth   = rayPosVS.z;

        /// @note レイが面の奥へ入り込み、かつ厚みの内側なら遮蔽。厚みで打ち切らないと、
        ///       手前の細い物体の «奥» をかすめただけのレイまで影にしてしまう。
        /// @note 余裕は厚みの半分で頭打ちにする。距離に比例させたままだと遠景で厚みを超え、影が出なくなる。
        const float penetration = rayDepth - sceneDepth;
        const float selfBias    = min(rayDepth * kSelfShadowBias, contactShadowThick * 0.5f);
        if (penetration > selfBias && penetration < contactShadowThick)
        {
            factor = 0.0f;
            break;
        }
    }

    OutputContactShadow[id.xy] = lerp(1.0f, factor, contactShadowStrength);
}
