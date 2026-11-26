/// @file    WaterfallComponent.hpp
/// @brief   保存対象の滝接続と、再生成できる描画部品。
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Asset/Water/WaterfallFluidClip.hpp>
#include <Engine/Scene/Components/Rendering/TransientGpuState.hpp>
#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Engine/Scene/Water/Waterfall/Waterfall.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <array>
#include <future>
#include <string>

namespace fbzz::scene {
struct WaterfallRuntime {
    std::array<EntityID, 6> parts{}; ///< 滝の描画と粒子演出のために生成した子エンティティの配列。
    WaterfallGeometry geometry; ///< 上流・着水点と滝の断面から解決した描画および流体用の形状。
    math::Vector3 midFall; ///< 落下途中の演出を配置するためにメッシュ生成で求めたワールド座標。
    std::uint64_t terrainRevision = 0; ///< 地表の影響を再評価するために保持する地形の更新版。
    math::Vector4 groundProbeBounds{}; ///< 滝の生成で地面を照会したワールド XZ の外接矩形。成分順は最小 X、最小 Z、最大 X、最大 Z。
    bool hasGroundProbes = false; ///< 地表への有限な照会点があり groundProbeBounds を使用できる場合に true。
    bool groundProbesFinite = true; ///< 照会座標の有限性を確認し、有効な矩形で地形変更を絞れる場合に true。
    float spray = -1.0f; ///< 生成済みのしぶきの強度。負値は未設定。
    float mist = -1.0f; ///< 生成済みの水煙の強度。負値は未設定。
    bool prepared = false; ///< 現在の接続と地形の入力に対して滝の生成を試みた。
    bool valid = false; ///< 現在の入力から描画できる滝の形状を生成できた。
    std::string status; ///< 滝の接続または描画部品の準備状況を Inspector に表示する文字列。
    std::string fluidStatus; ///< 着水部の流体クリップの読み込みまたは再生状況を表示する文字列。
    std::string playingFluidClip; ///< 着水部で現在準備または再生している .wfall 参照。
    std::string resolvedFluidPath; ///< アセット参照を解決した流体クリップのファイルパス。
    std::uint64_t fluidStamp = 0; ///< 非同期読み込みで照合する、焼き込み入力と着水部クリップの対応を示す署名。
    std::uint64_t fluidInputRevision = 0; ///< 滝の形状や地表条件を含む流体入力の版。
    std::uint64_t fluidAssetRevision = 0; ///< クリップの再試行を判断するために記録したアセットデータベースの版。
    float fluidTime = 0.0f; ///< ループ中の流体クリップ内の再生時刻（秒）。
    int fluidFrame = -1; ///< 最後に反映した流体クリップのフレーム添字。負値は未反映。
    int fluidFrameCount = 0; ///< 読み込み済み着水部クリップのフレーム総数。
    int fluidFps = 24; ///< 着水部クリップの再生フレームレート（fps）。
    bool fluidLoadFailed = false; ///< 現在の参照と版で着水部クリップの読み込みに失敗した。
    bool fluidViewDemand = false; ///< 可視ビューが流体クリップの再生を要求している場合に true。
    std::future<asset::WaterfallFluidFrame> fluidFrameJob; ///< 要求した 1 フレームをバックグラウンドで読み込む処理とその結果。
};

struct WaterfallComponent : TransientGpuState<WaterfallRuntime> {
    bool enabled = true; ///< 上流と下流を結ぶ滝の描画部品および演出を更新する。
    EntityRef upstream; ///< 落水を供給する WaterComponent の参照。
    EntityRef downstream; ///< 着水先となる WaterComponent の参照。
    bool upstreamAtStart = false; ///< 上流がスプライン川のとき先頭を落水端に選ぶ。false は末尾。
    bool downstreamAtStart = true; ///< 下流がスプライン川のとき先頭を着水端に選ぶ。false は末尾。
    bool rockAttached = false; ///< 岩に沿う落下形状と地表との間隔を適用する。
    float breakupAmount = 1.0f; ///< 水の幕の分裂や見た目の乱れを調整する倍率。
    float sprayAmount = 1.0f; ///< 着水時のしぶき生成の強さ。
    float mistAmount = 1.0f; ///< 滝の周囲に出す水煙の生成の強さ。
    std::string fluidClip; ///< 着水部で再生するローカル流体水面クリップ（.wfall）の参照。
    float fluidSize = 8.0f; ///< 着水部を流体として計算する領域の幅（m）。
    int fluidGrid = 40; ///< 着水部の流体計算で使用する格子の解像度。
    int fluidEmission = 850; ///< 水の幕の下部から 1 秒あたりに放出する液体粒子の数。
    int fluidFrames = 48; ///< 24 fps の着水部クリップに記録するフレーム数。
    std::vector<EntityRef> fluidRocks; ///< 着水部の流体計算で障害物として使うオブジェクトの参照列。

    const char* GetTypeName() const { return "Waterfall"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.RefField("upstream", upstream, "WaterComponent");
        r.Field("upstreamAtStart", upstreamAtStart);
        r.RefField("downstream", downstream, "WaterComponent");
        r.Tooltip("For a lake or rectangular pool, place the Waterfall object at the intended impact point.");
        r.Field("downstreamAtStart", downstreamAtStart);
        r.Field("rockAttached", rockAttached);
        r.FloatRange("breakupAmount", breakupAmount, 0.0f, 2.0f);
        r.FloatRange("sprayAmount", sprayAmount, 0.0f, 2.0f);
        r.FloatRange("mistAmount", mistAmount, 0.0f, 2.0f);
        r.FileField("fluidClip", fluidClip, ".wfall", "Generated local liquid surface clip.");
        r.Tooltip("Width of the simulated impact region in meters.");
        r.FloatRange("fluidSize", fluidSize, 1.0f, 48.0f);
        r.IntRange("fluidGrid", fluidGrid, 24, 64);
        r.Tooltip("Liquid particles emitted per second across the lower curtain.");
        r.IntRange("fluidEmission", fluidEmission, 100, 3000);
        r.Tooltip("Number of frames in the 24 FPS surface clip.");
        r.IntRange("fluidFrames", fluidFrames, 8, 120);
        r.RefListField("fluidRocks", fluidRocks, "");
        r.Readonly("status", status);
        r.Readonly("fluidStatus", fluidStatus);
    }
};

struct WaterfallPart {
    std::size_t index = 0; ///< WaterfallRuntime::parts でこの描画部品を識別する添字。
    std::uint32_t farIndexStart = 0; ///< 公開済みメッシュで遠景用の三角形が始まるインデックスバッファ内の位置。
    std::uint32_t pendingFarIndexStart = 0; ///< 次のメッシュ公開時に farIndexStart へ適用する位置。
};
}
