/// @file FroxelInject.cs.hlsl
/// @brief 視錐台フロクセルへ光の散乱と消散を書き込む (フロクセル霧のパス 1)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// 1 スレッド = 1 フロクセル。そのフロクセル中心のワールド座標で、
//   ・Directional (カスケードシャドウで遮蔽判定)
//   ・点光源 / スポット / 大きさを持つ光源 (専用アトラスのシャドウと Cookie 込み)
// を Henyey-Greenstein 位相で足し、散乱色と消散係数を RGBA へ書く。
//
// ライトの供給元は PS と同じ ClusteredLights.hlsli 経由。Clustered ならそのフロクセルが
// 属するクラスタの持ちぶんだけを、Linear なら統合配列を全数走査する。
//
// WHY レガシー経路 (b3) に留めないか: あちらは点 8 / スポット 4 / 形状 4 が上限で、
//      ネオン管を何十本も置いたシーンでは大半のライトが霧に映らない。しかも全フロクセルが
//      全ライトを無条件に評価するため、上限を上げるとフロクセル数 (既定 160x90x64) と
//      ライト数の積で費用が膨らむ。クラスタで絞る方が正しく、かつ速い。

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

    // 標本点はスライス内でジッターさせる (板を消すため)。
    // 一方「このフロクセルが代表する場所」はジッター無しの中心で、フレーム間で動かない。
    // ライトの取捨選択と履歴の引き直しは後者を使う。
    const float3 worldPos     = FBZZ_FroxelWorldPos(id);
    const float3 froxelCenter = FBZZ_FroxelCenter(id);
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

    // ---- 点光源 / スポット / 大きさを持つ光源 ----
    // FBZZ_EvalPunctual が減衰・コーン・シャドウ・Cookie を畳んだ intensity を返す。
    // 面光源 / 球 / 管もそのまま通る。
    //
    // Clustered 経路は svXY からクラスタのタイルを引く。フロクセルの XY は画面を
    // froxelGridX × froxelGridY に等分したタイルそのもの (FBZZ_FroxelWorldPos の uv と同じ) なので、
    // 画面解像度を持っていなくてもクラスタ側のタイル寸法から画素座標を復元できる。
    // WHY b13 へ screenSize を足さないか: clusterTilePx = screenSize / クラスタ分割数 なので
    //      既に等価な情報が b9 にある。両方に置くと、どちらが正本か分からない状態になる。
    const float2 screenPx = clusterTilePx * float2(FBZZ_CLUSTER_GRID_X, FBZZ_CLUSTER_GRID_Y);
    const float2 svXY = (float2(id.xy) + 0.5f)
                      / float2(max(froxelGridX, 1u), max(froxelGridY, 1u)) * screenPx;

    // フロクセルは点ではなく体積を代表している。逆二乗を中心 1 点で標本化すると、
    // 光源がそのフロクセルの内側に来た瞬間に値が跳ね上がる (LightAttenuation の下限は
    // 0.1m 相当で、サーフェス向けの値)。さらにスライス内のジッターで標本点が毎フレーム
    // 最大 1 スライスぶん動くため、光源のすぐ近くのフロクセルだけ値が何十倍も振れる。
    //
    // 指数分割なので奥ほどスライスが厚い。GreenWare の設定 (near 0.1 / far 90 / 64 段) では
    // 38m 地点で 1 スライスが約 4.3m あり、そこに置いた管ライトの周りが激しくちらつく。
    //
    // そこでフロクセルが代表する体積の半径を逆二乗の下限として使い、「フロクセルの縁より
    // 近くには寄れない」ものとして頭打ちにする。手前の薄いスライスでは上限が十分に高く、
    // 従来どおり点として扱われる。
    const float froxelRadius = max(
        (FBZZ_FroxelSliceToViewZ(float(id.z) + 1.0f)
         - FBZZ_FroxelSliceToViewZ(float(id.z))) * 0.5f, 0.05f);
    const float maxAttenuation = 1.0f / (froxelRadius * froxelRadius);

    // 顔ぶれはジッター無しの中心で決め、評価はジッター位置で行う。
    // ジッター位置で引くと、クラスタ境界のフロクセルだけライトが毎フレーム入れ替わる。
    FBZZ_PUNCTUAL_BEGIN_AT(froxelCenter, worldPos, svXY, -V)
        const float phase = FBZZ_HenyeyGreenstein(dot(-V, ps.L), froxelAnisotropy);

        // WHY intensity ではなく intensityNoCosine か: 霧は dot(N, L) を掛けない。
        //     大きさを持つ光源の ps.intensity は、呼び出し側がそれを掛け直す前提で
        //     先に割ってあるので、そのまま使うと最大 20 倍明るくなる。
        // ps.intensityNoCosine = lt.intensity * 窓 * 減衰 で、窓は 1 以下。したがって
        // lt.intensity * maxAttenuation が「この体積で取りうる減衰の上限」になる。
        // Area だけは逆二乗を掛けない (減衰を形態係数が持っている) ので対象外。
        const float intensity = (_fbzzLight.type == FBZZ_LIGHT_TYPE_AREA)
            ? ps.intensityNoCosine
            : min(ps.intensityNoCosine, max(_fbzzLight.intensity, 0.0f) * maxAttenuation);

        inScatter += ps.color * intensity * phase;
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
    //
    // WHY ジッター位置ではなく中心を渡すか: 履歴はフロクセル id.z をテクセル中心
    //     (id.z + 0.5) / gridZ に格納している。ジッター位置を渡すと uvw.z が
    //     (id.z + jitter) / gridZ になり、静止カメラでも毎フレーム違う配合で
    //     自分と隣スライスを三線形ブレンドして読むことになる。これでは平均が収束せず、
    //     読むたびにジッター由来の変動を注ぎ直す。中心を渡せば自分自身を正確に読み戻す。
    if (froxelHistoryValid != 0u && froxelHistoryBlend < 1.0f) {
        float3 histUVW;
        if (FBZZ_FroxelHistoryUVW(froxelCenter, histUVW)) {
            const float4 hist = gFroxelHistory.SampleLevel(gFroxelHistorySamp, histUVW, 0.0f);
            // 履歴が NaN / 負に転んでいたら捨てる (初回フレームの未初期化対策)。
            if (all(isfinite(hist)) && all(hist >= 0.0f))
                result = lerp(hist, result, froxelHistoryBlend);
        }
    }

    gFroxelScatter[id] = result;
}
