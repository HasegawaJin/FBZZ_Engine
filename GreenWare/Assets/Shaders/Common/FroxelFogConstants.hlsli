/// @file FroxelFogConstants.hlsli
/// @brief フロクセル ボリューメトリック フォグの定数と座標変換
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// LAYOUT: Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp の
//         FroxelFogCB と完全に一致させること (224 bytes)。
//
// WHY 画面空間レイマーチ (VolumetricLightPass) と別に持つか:
//   あちらは 1 画素ごとにカメラからシーンまでを毎フレーム積分し直す。画素数ぶんの
//   レイマーチなので解像度に比例して重く、点光源を何本も入れるとすぐ破綻する。
//   フロクセルは視錐台を粗い 3D グリッドへ一度だけ焼いてから画面へ引くので、
//   コストが画面解像度から切り離される。ライトを増やしても効くのはグリッドの体積だけ。
#ifndef FROXEL_FOG_CONSTANTS_HLSLI
#define FROXEL_FOG_CONSTANTS_HLSLI

#include "Common/Binding.hlsli"

cbuffer FroxelFogConstants : register(CB_FROXEL_FOG)
{
    // NDC → ワールド。フロクセル中心のワールド座標を出すのに使う。
    float4x4 froxelInvViewProj;

    float3 froxelCameraPos;   float froxelNear;   // グリッドの手前端 [m]
    float3 froxelAlbedo;      float froxelFar;    // グリッドの奥端 [m]
    float3 froxelEmissive;    float froxelDensity; // 一様な消散係数 [1/m]

    float froxelAnisotropy;    // Henyey-Greenstein の g。正で前方散乱
    float froxelHeightFalloff; // 高度による密度減衰 [1/m]。0 で無効
    float froxelHeightStart;   // 減衰の基準高度 [world Y]
    float froxelJitter;        // スライス内のサンプル位置ずらし [0,1)

    uint  froxelGridX;
    uint  froxelGridY;
    uint  froxelGridZ;
    float froxelAmbient;       // 環境光が霧へ寄与する倍率

    float4x4 froxelPrevViewProj; // 前フレームのビュー射影 (履歴の引き直し用)
    float froxelHistoryBlend;    // 今フレームの寄与率。小さいほど滑らか
    uint  froxelHistoryValid;    // 0 のフレームは履歴を使わない
    float2 _froxelPad;
};

// スライス番号 → ビュー空間深度。
//
// WHY 指数分割か: 霧はカメラ近傍ほど画面上の面積を大きく占める。等間隔に切ると
//     手前 1 スライスが数メートルを覆い、目の前の光柱が階段状になる。
//     クラスタライトの Z 分割と同じ考え方で、near 側を細かく取る。
float FBZZ_FroxelSliceToViewZ(float slice)
{
    const float t = slice / max(float(froxelGridZ), 1.0f);
    return froxelNear * pow(max(froxelFar / max(froxelNear, 1e-4f), 1.0f), t);
}

// ビュー空間深度 → スライス番号 (小数)。FBZZ_FroxelSliceToViewZ の逆関数。
float FBZZ_FroxelViewZToSlice(float viewZ)
{
    const float ratio = max(froxelFar / max(froxelNear, 1e-4f), 1.0001f);
    const float t = log(max(viewZ, froxelNear) / max(froxelNear, 1e-4f)) / log(ratio);
    return saturate(t) * float(froxelGridZ);
}

// フロクセルのワールド座標。
//   coord       : フロクセル座標 (x, y, z)
//   sliceOffset : スライス内のどこを標本化するか [0,1)
float3 FBZZ_FroxelWorldPosAt(uint3 coord, float sliceOffset)
{
    // XY はタイル中心。Z は呼び出し側が決めた位置。
    const float2 uv = (float2(coord.xy) + 0.5f)
                    / float2(max(froxelGridX, 1u), max(froxelGridY, 1u));
    const float viewZ = FBZZ_FroxelSliceToViewZ(float(coord.z) + sliceOffset);

    // ビュー深度 → NDC 深度 (DirectX の 0..1)。逆投影に食わせる形へ戻す。
    const float ndcZ = (froxelFar * (viewZ - froxelNear))
                     / max(viewZ * (froxelFar - froxelNear), 1e-6f);

    const float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, saturate(ndcZ), 1.0f);
    const float4 world = mul(ndc, froxelInvViewProj);
    return world.xyz / world.w;
}

// フロクセルの標本点 (jitter 込み)。密度・位相・影を引くのはこの位置。
// WHY ずらすか: グリッドが粗いので、固定位置で標本化するとスライス境界が
//     「霧の中の板」として見える。フレームごとに位置を振ればちらつきへ変わり、
//     動いている絵の中では板より遥かに目立たなくなる。
float3 FBZZ_FroxelWorldPos(uint3 coord)
{
    return FBZZ_FroxelWorldPosAt(coord, froxelJitter);
}

// ジッターを含まないフロクセル中心。フレーム間で動いてほしくない用途に使う。
// WHY 要るか: 「どのライトを評価するか」をジッター位置で決めると、クラスタ境界の
//     フロクセルだけ顔ぶれが毎フレーム入れ替わり、ちらつきとして見えてしまう。
//     位置のばらしが効いてほしいのは密度と影であって、ライトの取捨選択ではない。
float3 FBZZ_FroxelCenter(uint3 coord)
{
    return FBZZ_FroxelWorldPosAt(coord, 0.5f);
}

// ワールド座標 → 前フレームのフロクセル UVW ([0,1]^3)。
//
// XY は前フレームのクリップ座標から、Z はビュー深度を指数分割の逆関数へ通して出す。
// 視錐台の外や near より手前へ落ちた場合は履歴が無いので false を返す。
//
// IMPORTANT: worldPos には FBZZ_FroxelCenter (ジッター無し) を渡すこと。
//     履歴はフロクセル id.z をテクセル中心 (id.z + 0.5) / gridZ に格納している。
//     ジッター位置を渡すと uvw.z が (id.z + jitter) / gridZ になり、静止カメラでも
//     毎フレーム違う配合で自分と隣スライスを三線形ブレンドして読むことになる。
//     時間平均が収束せず、読むたびにジッター由来の変動を注ぎ直すことになる。
//
// WHY 同じフロクセル座標で履歴を引かないか: グリッドはカメラに貼り付いているので、
//     カメラが動くと同じ (x,y,z) は前フレームと別のワールド位置を指す。
//     引き直さないと霧が視界の動きに引きずられて尾を引く。
bool FBZZ_FroxelHistoryUVW(float3 worldPos, out float3 uvw)
{
    uvw = float3(0.0f, 0.0f, 0.0f);
    const float4 clip = mul(float4(worldPos, 1.0f), froxelPrevViewProj);
    if (clip.w <= 1e-5f) return false;

    const float2 ndc = clip.xy / clip.w;
    if (any(abs(ndc) > 1.0f)) return false;

    // clip.w は前フレームのビュー空間深度。
    if (clip.w < froxelNear || clip.w > froxelFar) return false;

    uvw.x = ndc.x * 0.5f + 0.5f;
    uvw.y = 0.5f - ndc.y * 0.5f;
    uvw.z = FBZZ_FroxelViewZToSlice(clip.w) / max(float(froxelGridZ), 1.0f);
    return true;
}

// Henyey-Greenstein 位相関数。cosTheta は入射方向と視線方向の内積。
float FBZZ_HenyeyGreenstein(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    // 1/(4π) 正規化込み。denom が 0 に落ちるのは g=1 の完全前方散乱だけ。
    return (1.0f - g2) / (4.0f * 3.14159265f * max(pow(max(denom, 1e-4f), 1.5f), 1e-4f));
}

// このワールド座標での消散係数 [1/m]。高度減衰込み。
float FBZZ_FroxelExtinction(float3 worldPos)
{
    if (froxelHeightFalloff <= 0.0f) return froxelDensity;
    const float h = max(worldPos.y - froxelHeightStart, 0.0f);
    return froxelDensity * exp(-h * froxelHeightFalloff);
}

#endif // FROXEL_FOG_CONSTANTS_HLSLI
