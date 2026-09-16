/// @file DecalMask.hlsl
/// @brief 可視サーフェスのレイヤー番号をデカール受信バッファへ書く (静的メッシュ)
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 「除外オブジェクトを白く塗る」ではなくレイヤー番号か:
///   白塗りだと receiverLayerMask ごとにバッファを作り直すことになり、マスクを
///   持つデカール 1 個につきシーン全体を 1 回描き直していた。レイヤー番号を
///   書いておけば 1 フレーム 1 回で済み、判定はデカール側のビットテストになる。
///
/// WHY 深度で棄却するか:
///   このバッファは「その画素に見えている面がどのレイヤーか」を表す。深度を見ずに
///   描くと、壁の裏に隠れた非受信オブジェクトが手前の壁のデカールを削り取る。
///   Decal.hlsl は深度から復元した可視サーフェスへ投影するので、こちらも可視
///   サーフェスだけを書かないと指すものが食い違う。
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSceneDepth, TEX_DEPTH_SLOT);

// DecalPass がオブジェクトごとに埋める。
// LAYOUT: RenderPassContext.hpp の DecalReceiverCB と一致させること。
cbuffer DecalReceiverConstants : register(CB_DECAL)
{
    float receiverLayerEncoded; // レイヤー番号 + 1 (0 はクリア値 = 未描画)
    float3 _receiverPad;
};

struct DecalMaskPixelInput
{
    float4 pos : SV_POSITION;
};

// 可視サーフェス判定の許容差。深度の非線形性に引きずられないようビュー空間で取る。
// WHY 広めに取るか: 狭すぎると頂点計算のわずかな差で自分自身の画素まで落ち、
//     マスクが黙って無効になる。広すぎたときの副作用は「数 cm 裏の面も
//     可視扱い」に留まるので、こちら側へ倒す。
float DecalReceiverDepthTolerance(float viewDepth)
{
    return max(0.01f, viewDepth * 0.002f);
}

DecalMaskPixelInput VSMain(float3 pos : POSITION)
{
    DecalMaskPixelInput output;
    // WHY GBuffer.hlsl と同じ積の順序にするか: 順序が違うと丸めが変わり、同じ頂点でも
    //     深度がわずかにずれる。可視判定はその一致を当てにしている。
    output.pos = mul(mul(float4(pos, 1.0f), world), viewProjection);
    return output;
}

float4 PSMain(DecalMaskPixelInput input) : SV_Target
{
    float sceneDepth = texSceneDepth.Load(int3((int)input.pos.x, (int)input.pos.y, 0)).r;
    float sceneView  = LinearizeDepth(sceneDepth, nearZ, farZ, isOrthographic);
    float fragView   = LinearizeDepth(input.pos.z, nearZ, farZ, isOrthographic);
    if (fragView > sceneView + DecalReceiverDepthTolerance(sceneView))
        discard;

    return float4(receiverLayerEncoded, 0.0f, 0.0f, 1.0f);
}
