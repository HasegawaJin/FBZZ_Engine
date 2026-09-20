/// @file    LightProbeVolumeComponent.hpp
/// @brief   箱の中に格子状のプローブを並べ、周囲の間接光を L2 球面調和で焼いて拡散環境光を差し替えるボリューム。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 箱はワールド軸に沿い、中心は worldPosition。回転とスケールは見ない (ReflectionProbe の箱と同じ扱い)。
/// @see Docs/design/light-probe-gi.md
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <array>
#include <cstdint>

namespace fbzz::scene {

struct LightProbeVolumeComponent {
    bool          enabled         = true;
    math::Vector3 boxExtents      = { 5.0f, 2.5f, 5.0f }; ///< 箱の半分の大きさ [m]
    int           probeCountX     = 8;
    int           probeCountY     = 4;
    int           probeCountZ     = 8;
    float         intensity       = 1.0f;  ///< 拡散 GI の倍率
    float         normalBias      = 0.25f; ///< 引く点を法線方向へずらす距離 [m]。壁に埋まったプローブの暗さが漏れるのを弱める
    float         edgeFade        = 1.0f;  ///< 箱の縁で IBL へ戻していく幅 [m]。0 で境目が段になる
    float         specularOcclusion = 1.0f; ///< プローブの暗さを鏡面 IBL にも掛ける強さ [0,1]。屋内に残る空の映り込みを消す
    float         deringing       = 0.5f;  ///< SH のリンギング抑制 [0,1]。強い日だまりの反対側に出る暗い輪を弱める
    bool          rejectInsideGeometry = true; ///< 形状の中に埋まったプローブを捨て、周りの値で埋める (壁際の黒いにじみ対策)
    int           captureResolution = 32;  ///< プローブ 1 面の解像度 [px]。拡散は低周波なので 32 で足りる
    int           probesPerFrame  = 8;     ///< 1 フレームに焼くプローブ数。1 プローブ = 6 面ぶんのシーン描画
    int           bounces         = 2;     ///< 焼く回数。2 回目以降は前回の結果を受けた面を描くので多重反射になる
    bool          realtimeUpdate  = false; ///< 焼き終えたら最初から焼き直し続ける (昼夜や動く光源に追従)
    bool          bakeRequested   = false; ///< Inspector / Script が立てる。次フレームから焼き直す

    /// @name ランタイム (保存しない)
    /// @note GPU ハンドルは Scene の瞬間値で、保存すると古いハンドルを復元してしまう。Play の往復で作り直され、自動で焼き直す。
    /// @{
    renderer::ResourceHandle<renderer::TextureTag> runtimeVolume;    ///< 描画が読む (無効プローブを埋めた後)
    renderer::ResourceHandle<renderer::TextureTag> runtimeRawVolume; ///< 射影 CS が書く生の値
    std::array<renderer::ResourceHandle<renderer::RenderTargetTag>, 6> runtimeFaces{};
    std::array<renderer::ResourceHandle<renderer::RenderTargetTag>, 6> runtimeFacing{}; ///< 表裏判定の面
    uint64_t runtimeSettingsKey = 0;       ///< 焼き結果を左右する設定の要約。変わったら焼き直す
    std::array<int, 3> runtimeGrid{};      ///< runtimeVolume を作ったときの格子
    math::Vector3 runtimeBoxMin{};         ///< 焼いたときの箱。動いたら焼き直す
    math::Vector3 runtimeBoxSize{};
    int      runtimeFaceSize  = 0;
    int      runtimeCursor    = 0;         ///< 次に焼くプローブの通し番号
    int      runtimePass      = 0;         ///< 何回目の焼き (0 始まり)
    bool     runtimeBaking    = false;
    bool     runtimeReady     = false;     ///< 1 回目の焼きが全プローブ終わったか (それまでは描画に使わない)
    /// @}

    [[nodiscard]] std::array<int, 3> ClampedGrid() const
    {
        return { std::clamp(probeCountX, 1, 64), std::clamp(probeCountY, 1, 32), std::clamp(probeCountZ, 1, 64) };
    }
    [[nodiscard]] int ProbeCount() const
    {
        const auto grid = ClampedGrid();
        return grid[0] * grid[1] * grid[2];
    }

    const char* GetTypeName() const { return "LightProbeVolume"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",           enabled);
        r.Field("boxExtents",        boxExtents);
        r.Field("probeCountX",       probeCountX);
        r.Field("probeCountY",       probeCountY);
        r.Field("probeCountZ",       probeCountZ);
        r.Field("intensity",         intensity);
        r.Field("normalBias",        normalBias);
        r.Field("edgeFade",          edgeFade);
        r.Field("specularOcclusion", specularOcclusion);
        r.Field("deringing",         deringing);
        r.Field("rejectInsideGeometry", rejectInsideGeometry);
        r.Field("captureResolution", captureResolution);
        r.Field("probesPerFrame",    probesPerFrame);
        r.Field("bounces",           bounces);
        r.Field("realtimeUpdate",    realtimeUpdate);
    }
};

} // namespace fbzz::scene
