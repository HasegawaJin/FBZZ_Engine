/// @file ExposureAverage.cs.hlsl
/// @brief ヒストグラムから露出を決めて時間順応させる (自動露出のパス 2)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// 1 グループ 256 スレッド = ビン数。並列リダクションで加重平均を出したあと、
// スレッド 0 が前フレームの値から指数的に近づけて書き戻す。
//
// WHY 同じ CS でヒストグラムを 0 に戻すか: ヒストグラムは毎フレーム作り直すので、
//     どこかで必ずクリアが要る。ここで読んだ直後に消せば、専用のクリアディスパッチも
//     「前フレームのゴミが混ざる」タイミングも生まれない。

#include "PostProcess/Color/ExposureCommon.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_RWSBUFFER_T(uint, gHistogram, UAV_CLUSTER_LIGHTS_SLOT); // u2
FBZZ_RWSBUFFER_T(float, gExposure, UAV_GPU_SORT_SLOT);       // u3

// 各ビンの画素数。スレッド 0 が累積和を取るための作業領域。
//
// WHY 並列リダクションにしないか: 明部 / 暗部の切り捨ては「このビンより下に
//     何画素あるか」で決まる累積和で、素直に並列化するとプレフィックス和の
//     実装が要る。1 フレームに 1 グループしか走らないパスなので、
//     256 回の直列ループの方が短くて読める。
groupshared float s_count[FBZZ_EXPOSURE_BINS];

[numthreads(FBZZ_EXPOSURE_BINS, 1, 1)]
void CSMain(uint groupIndex : SV_GroupIndex)
{
    const uint bin = groupIndex;
    s_count[bin]   = float(gHistogram[bin]);
    GroupMemoryBarrierWithGroupSync();

    if (groupIndex == 0u) {
        float total = 0.0f;
        for (uint i = 0u; i < FBZZ_EXPOSURE_BINS; ++i)
            total += s_count[i];

        const float lowCut  = total * saturate(exposureLowPercent);
        const float highCut = total * saturate(exposureHighPercent);

        float seen        = 0.0f;
        float weightedSum = 0.0f;
        float usedCount   = 0.0f;
        // ビン 0 (実質黒) は最初から除く。夜のシーンで画面の大半が黒でも、
        // 見えている物の明るさに露出を合わせたい。
        for (uint b = 1u; b < FBZZ_EXPOSURE_BINS; ++b) {
            const float c = s_count[b];
            if (c <= 0.0f) continue;

            // このビンのうち [lowCut, highCut] の帯に入る画素数だけを採用する。
            const float binStart = seen;
            const float binEnd   = seen + c;
            seen = binEnd;

            const float used = max(min(binEnd, highCut) - max(binStart, lowCut), 0.0f);
            if (used <= 0.0f) continue;

            weightedSum += FBZZ_BinToLuminance(b) * used;
            usedCount   += used;
        }

        // 帯に 1 画素も入らない (真っ黒 / 真っ白) ときは順応させない。
        // WHY 既定値へ倒さないか: 一瞬の全白フラッシュで露出が飛ぶと、
        //     明けたあと数秒かけて戻ることになる。据え置きの方が破綻が小さい。
        float target = (usedCount > 0.0f) ? (weightedSum / usedCount) : -1.0f;

        float current = gExposure[0];
        if (exposureReset != 0u || current <= 0.0f)
            current = (target > 0.0f) ? target : 1.0f;
        else if (target > 0.0f) {
            // 指数順応。1 - exp(-speed*dt) はフレームレートに依存しない補間係数。
            const float speed = (target > current) ? exposureSpeedUp : exposureSpeedDown;
            const float t     = saturate(1.0f - exp(-max(speed, 0.0f) * max(exposureDeltaTime, 0.0f)));
            current = lerp(current, target, t);
        }

        // 露出そのものではなく「順応済みの平均輝度」を保存する。
        // WHY: 補正量 (compensation) やクランプは絵作りのパラメーターで、
        //      毎フレーム変わりうる。順応の状態と混ぜると、スライダーを動かした瞬間に
        //      順応が巻き戻ったように見える。
        gExposure[0] = max(current, 1e-5f);
    }

    GroupMemoryBarrierWithGroupSync();
    // 次フレームぶんをクリアする。読み終わった後であることが同期で保証されている。
    gHistogram[bin] = 0u;
}
