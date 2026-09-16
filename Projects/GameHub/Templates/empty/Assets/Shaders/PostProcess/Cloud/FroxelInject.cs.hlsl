/// @file FroxelInject.cs.hlsl
/// @brief 視錐台フロクセルへ光の散乱と消散を書き込む (フロクセル霧のパス 1)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// 1 スレッド = 1 フロクセル。そのフロクセル中心のワールド座標で、
//   ・Directional (カスケードシャドウで遮蔽判定)
//   ・点光源 / スポット (専用アトラスのシャドウと Cookie 込み)
// を Henyey-Greenstein 位相で足し、散乱色と消散係数を RGBA へ書く。
//
// NOTE: 点光源の供給は b3 の固定長配列 (点 8 / スポット 4) 経路のみ。
//       クラスタリストは PS 専用スロット (t29/t30) にあり、CS からは読めない。
//       霧に効くのは画面を広く照らす主要なライトなので、実用上ここが上限になる。

// クラスタ占有のヒートマップは float4 を返して打ち切る作りで、void の CS では通らない。
// include より前に立てておく必要がある。
#define FBZZ_PUNCTUAL_NO_DEBUG 1

#include "Common/Constants.hlsli"
#include "Common/FroxelFogConstants.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Platform/Backend.hlsli"

Texture2D<float>       texShadow  : register(TEX_SHADOW);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);

RWTexture3D<float4> gFroxelScatter : register(UAV_FROXEL_SCATTER);

// 前フレームの散乱ボリューム。Z スライスのジッターで出るちらつきを均すために読む。
Texture3D<float4> gFroxelHistory     : register(TEX_FROXEL_HISTORY);
SamplerState      gFroxelHistorySamp : register(SAMPLER_FROXEL_HISTORY);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= froxelGridX || id.y >= froxelGridY || id.z >= froxelGridZ)
        return;

    const float3 worldPos = FBZZ_FroxelWorldPos(id);
    const float  extinction = FBZZ_FroxelExtinction(worldPos);

    // 密度 0 の領域は光を散らさない。奥のスライスまで無駄に回さないよう即書いて抜ける。
    if (extinction <= 0.0f) {
        gFroxelScatter[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 視線方向 (フロクセル → カメラ)。位相関数の基準。
    const float3 V = SafeNormalize(froxelCameraPos - worldPos, float3(0.0f, 1.0f, 0.0f));

    float3 inScatter = froxelEmissive;

    // ---- Directional ----
    // WHY ComputeShadow をそのまま呼ぶか: カスケードの選択とアトラス矩形への写像を
    //     ここへ書き写すと、Directional の影の規則が 2 か所に分かれる。
    //     N には -V を渡す。霧に法線は無いが、スロープバイアスの入力として
    //     「視線に正対する面」とみなすのが最も無難 (バイアスが最小になる)。
    {
        const float3 L = SafeNormalize(-lightDir, float3(0.0f, 1.0f, 0.0f));
        const float shadow = ComputeShadow(texShadow, sampShadow, worldPos,
                                           lightViewProjection, shadowMapTexelSize,
                                           shadowBias, -V, L);
        const float phase = FBZZ_HenyeyGreenstein(dot(-V, L), froxelAnisotropy);
        inScatter += lightColor * lightIntensity * shadow * phase;
    }

    // ---- 点光源 / スポット ----
    // FBZZ_EvalPunctual が減衰・コーン・シャドウ・Cookie を畳んだ intensity を返す。
    // 面光源 / 球 / 管もそのまま通る。
    FBZZ_PUNCTUAL_BEGIN(worldPos, float2(0.0f, 0.0f), -V)
        const float phase = FBZZ_HenyeyGreenstein(dot(-V, ps.L), froxelAnisotropy);
        inScatter += ps.color * ps.intensity * phase;
    FBZZ_PUNCTUAL_END

    // 環境光。霧が完全な黒に落ちないための下駄で、屋外の遠景でとくに効く。
    inScatter += ambientColor * froxelAmbient;

    // 散乱アルベドと密度を掛けて「このフロクセルが視線へ返す光」にする。
    // alpha には消散係数を生で入れる。積分パスがスライス厚と掛けて透過率を作る。
    float4 result = float4(inScatter * froxelAlbedo * extinction, extinction);

    // ---- 時間方向の蓄積 ----
    // FBZZ_FroxelWorldPos の Z ジッターは、粗いグリッドの「板」をフレーム間の
    // ちらつきへ置き換える。ここで前フレームと混ぜて初めて、そのちらつきが
    // 滑らかな見た目に均される。蓄積しないとジッターがそのまま画面に出る。
    if (froxelHistoryValid != 0u && froxelHistoryBlend < 1.0f) {
        float3 histUVW;
        if (FBZZ_FroxelHistoryUVW(worldPos, histUVW)) {
            const float4 hist = gFroxelHistory.SampleLevel(gFroxelHistorySamp, histUVW, 0.0f);
            // 履歴が NaN / 負に転んでいたら捨てる (初回フレームの未初期化対策)。
            if (all(isfinite(hist)) && all(hist >= 0.0f))
                result = lerp(hist, result, froxelHistoryBlend);
        }
    }

    gFroxelScatter[id] = result;
}
