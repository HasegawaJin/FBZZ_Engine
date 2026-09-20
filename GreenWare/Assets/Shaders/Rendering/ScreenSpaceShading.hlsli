/// @file ScreenSpaceShading.hlsli
/// @brief Forward のマテリアルが画面空間 AO / 接触影を受け取る口
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// WHY 要るか: SSAO / GTAO / 接触影は GBuffer から作られ、Deferred では
//     DeferredLighting の全画面パスがまとめて適用していた。Forward には
//     その合流点が無いため、同じ設定でも効果が丸ごと消えていた。
//     Forward でも GBuffer プリパスを描くようにしたので、あとは各マテリアルが
//     自分の画素で結果を引けば、4 パイプラインで絵が揃う。
//
// WHY 全画面で HDR へ掛け戻さないか: AO は環境光にだけ掛かる遮蔽項で、直接光や
//     エミッシブには掛からない。合成後の HDR へ一律に掛けると、光が直接当たっている
//     面まで暗くなり、AO ではなく「汚れ」に見える。マテリアル内で ao へ畳むのが正しい。
//
// NOTE: 自前で t24 を宣言している DeferredLighting のようなシェーダーは、
//       include より前に FBZZ_NO_SCREEN_SHADING を定義して丸ごと外す。
#ifndef SCREEN_SPACE_SHADING_HLSLI
#define SCREEN_SPACE_SHADING_HLSLI

#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"

#ifndef FBZZ_NO_SCREEN_SHADING

FBZZ_TEX2D_T(float4, gScreenAO, TEX_SCREEN_AO_SLOT);      // t23
FBZZ_TEX2D_T(float, gScreenContactShadow, TEX_CONTACT_SHADOW_SLOT); // t24

// FBZZ_ScreenAO — この画素の画面空間 AO [0,1]。1 = 遮蔽なし。
//
// screenAoStrength (b8) が 0 のときは 1 を返す。Deferred では DeferredLighting が
// 同じ AO を適用済みなので、そちらでは 0 を渡して二重適用を防ぐ。
//
// WHY Sample ではなく Load か: 共有ヘッダーから SamplerState を宣言すると、
//     s2 (Linear clamp) を自前の名前で持っているマテリアルシェーダーと二重定義になる。
//     AO はブラー済みの低周波なので、点取りでも実用上の差が出ない。
//     半解像度で焼かれている前提の座標変換だけ screenAoScale で吸収する。
float FBZZ_ScreenAO(float2 svXY)
{
    if (screenAoStrength <= 0.0f) return 1.0f;
    /// @note 半解像度は切り捨てで作るので、奇数の画面幅では最後の列が範囲外になる。範囲外の Load は 0 (真っ黒) を返す。
    uint aoW, aoH;
    gScreenAO.GetDimensions(aoW, aoH);
    const int2 texel = min(int2(svXY * screenAoScale), int2(aoW, aoH) - 1);
    const float ao = saturate(gScreenAO.Load(int3(texel, 0)).r);
    return lerp(1.0f, ao, saturate(screenAoStrength));
}

// FBZZ_ScreenContactShadow — この画素の接触影 [0,1]。1 = 遮蔽なし。
// 影の項へ掛けることで、シャドウマップの解像度では拾えない接地部の暗がりが出る。
float FBZZ_ScreenContactShadow(float2 svXY)
{
    if (screenContactShadowStrength <= 0.0f) return 1.0f;
    uint csW, csH;
    gScreenContactShadow.GetDimensions(csW, csH);
    const int2 texel = min(int2(svXY * screenContactShadowScale), int2(csW, csH) - 1);
    const float mask = saturate(gScreenContactShadow.Load(int3(texel, 0)));
    return lerp(1.0f, mask, saturate(screenContactShadowStrength));
}

#else // FBZZ_NO_SCREEN_SHADING

// 自前で同じスロットを使うシェーダー向けの素通し。
float FBZZ_ScreenAO(float2 svXY)            { return 1.0f; }
float FBZZ_ScreenContactShadow(float2 svXY) { return 1.0f; }

#endif // FBZZ_NO_SCREEN_SHADING

#endif // SCREEN_SPACE_SHADING_HLSLI
