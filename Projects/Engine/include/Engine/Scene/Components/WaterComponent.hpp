/// @file    WaterComponent.hpp
/// @brief   水面コンポーネント（ジオメトリ・水の種類 (.mat) の参照・浮力・着水）。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// 水の «種類» ── 色・さざ波・Gerstner 波・風への反応・水流 ── は materialPath が指す
/// .mat が丸ごと持つ。コンポーネントに残すのは «この 1 枚» ごとに違う量だけ。
/// WHY 波まで .mat に置くか: 色だけ .mat にあり波がコンポーネントにあると、Ocean.mat を
///     差しても湖の波のまま、という食い違いが起きる。水の種類は 1 ファイルで決まるべき。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Physics/BodyHandle.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace fbzz::scene {

// GerstnerWave — 水面を構成する 1 本の波のパラメータ。
// WHY: 水面全体をシミュレーションせず、少数の解析波を合成することで低コストに大きなうねりを表現する。
struct GerstnerWave {
    math::Vector2 direction  = { 1.0f, 0.0f }; // 進行方向。正規化は評価側で行う。
    float         amplitude  = 0.30f;          // 波高 A [m]
    float         wavelength = 10.0f;          // 波長 lambda [m]
    float         steepness  = 0.50f;          // 急峻度 Q [0,1]。1 を超えると波面が交差する。
};

// WaterComponent — シーン上の水面 1 面分。
struct WaterComponent {
    // ── ジオメトリ ──────────────────────────────────────────────────────────
    uint32_t resolutionX = 64;
    uint32_t resolutionZ = 64;
    float    extentX     = 100.0f;
    float    extentZ     = 100.0f;
    // チャンク分割数。水面を chunkCount×chunkCount のサブメッシュに分割し、
    // チャンク単位でフラスタムカリングする。
    uint32_t chunkCount = 4;

    // ── 波 (個体ごとの補正。波そのものは .mat が持つ) ──────────────────────
    bool  enableGerstnerWaves = true;
    /// .mat の振幅に掛ける倍率。同じ Ocean.mat を «凪の入り江» と «外洋» に使い分けるため。
    float waveAmplitudeScale  = 1.0f;

    // ── 物理 ────────────────────────────────────────────────────────────────
    bool  buoyancyEnabled = true;
    /// 完全に沈んだときの上向き加速度 [m/s^2]。重力 (9.8) を超えると浮く。
    float buoyancy        = 15.0f;
    /// 水中での速度減衰 [1/s]。水流があるときは «水に対する» 速度に掛かる。
    float waterDrag       = 2.0f;
    /// 浮力が届く水面からの深さ [m]。
    /// WHY 上限を置くか: 水面は厚みを持たない板なので、置いたままだと «水面の真下にある洞窟»
    ///     の中まで浮力が届いてしまう。
    float buoyancyDepth   = 10.0f;
    /// 剛体が水面を通過したときに波紋としぶきを出す。
    bool  splashEnabled   = true;

    // ── 水の種類 ────────────────────────────────────────────────────────────
    std::string materialPath;

    // ── 状態フラグ ──────────────────────────────────────────────────────────
    bool enabled   = true;
    bool meshDirty = true;
    bool foamDirty = true;
    bool texDirty  = true;

    // ── ランタイム (保存しない) ─────────────────────────────────────────────
    /// WaterSystem が毎フレーム «.mat の波 × 倍率 × 環境風» から組み立てた実効波。
    /// 描画・浮力・水中判定・スクリプトはすべてこれを読む。
    std::array<GerstnerWave, 4> waves = {};
    /// 水流の速度 [m/s] (ワールド XZ)。.mat の flowDirection × currentSpeed。
    math::Vector2 current = math::Vector2::ZERO;
    /// WaterSystem が毎フレーム書く «頂点グリッド 1 セルのワールド実寸» [m]。
    /// WHY: 刻めない波長の Gerstner 波を寝かせる判断に、描画 (GPU) と浮力 (CPU) が
    ///      同じ値を使う必要がある。片方だけ寝かせると、平らな水面の上で物が揺れる。
    math::Vector2 cellSize = math::Vector2::ZERO;
    physics::VolumeHandle volumeHandle;

    const char* GetTypeName() const { return "Water"; }

    /// 頂点グリッドで «刻めない» 波を寝かせる係数 [0,1]。Water.hlsl の WaveMeshFade と同じ式。
    ///
    /// WHY: Gerstner 波は頂点でしか評価されないので、1 波長あたり数セルしか取れない波は
    ///      山と谷がセル境界で入れ替わり、«もっと長い別の波» に化ける。海サイズの水面では
    ///      これが全面で起きる。刻めない波は消し、細かさは手続きさざ波に任せる。
    /// @param cell 頂点グリッド 1 セルのワールド実寸 [m]。0 のときはフェードしない。
    static float WaveMeshFade(float wavelength, math::Vector2 cell)
    {
        const float c = (std::max)(cell.x, cell.y);
        if (c <= 0.0f) return 1.0f;
        // smoothstep(2, 3.5, 1 波長あたりのセル数)。下限 2.0 は Nyquist。
        const float t = math::Clamp01((wavelength / c - 2.0f) / 1.5f);
        return t * t * (3.0f - 2.0f * t);
    }

    /// 水面の基準面からの高さ [m] を CPU で評価する。
    /// @param worldX, worldZ ワールド座標。
    /// WHY ワールド座標か: シェーダーは波の位相をワールド XZ で取る。水面の原点からの相対座標で
    ///     評価すると、水面を原点以外へ置いた瞬間に浮力・水中判定と描画の波がずれる。
    float GetSurfaceHeightAt(float worldX, float worldZ, float time) const
    {
        if (!enableGerstnerWaves) return 0.0f;

        float height = 0.0f;
        for (const GerstnerWave& wave : waves) {
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
            // 描画が寝かせた波は水面の形にも出ない。同じ係数を掛けないと、平らに見える
            // 遠くの海面の上で浮いている物だけが波に乗って上下する。
            const float fade = WaveMeshFade(wave.wavelength, cellSize);
            if (fade <= 0.0f) continue;
            const math::Vector2 dir = wave.direction.Normalized();
            const float k = math::TWO_PI / wave.wavelength;
            const float omega = std::sqrt(9.8f * k);
            const float phase = k * (dir.x * worldX + dir.y * worldZ) - omega * time;
            height += wave.amplitude * fade * std::sin(phase);
        }
        return height;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        const uint32_t oldResolutionX = resolutionX;
        const uint32_t oldResolutionZ = resolutionZ;
        const uint32_t oldChunkCount  = chunkCount;
        const float oldExtentX = extentX;
        const float oldExtentZ = extentZ;
        const std::string oldMaterialPath = materialPath;

        // IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        int resX = static_cast<int>((std::clamp)(resolutionX, 1u, 512u));
        int resZ = static_cast<int>((std::clamp)(resolutionZ, 1u, 512u));
        int chunks = static_cast<int>((std::clamp)(chunkCount, 1u, 64u));
        r.Field("resolutionX", resX);
        r.Field("resolutionZ", resZ);
        r.Field("chunkCount", chunks);
        r.Field("extentX", extentX);
        r.Field("extentZ", extentZ);
        r.Field("materialPath", materialPath);
        r.Field("enableGerstnerWaves", enableGerstnerWaves);
        r.Field("waveAmplitudeScale", waveAmplitudeScale);
        r.Field("buoyancyEnabled", buoyancyEnabled);
        r.Field("buoyancy", buoyancy);
        r.Field("waterDrag", waterDrag);
        r.Field("buoyancyDepth", buoyancyDepth);
        r.Field("splashEnabled", splashEnabled);

        resolutionX = static_cast<uint32_t>((std::clamp)(resX, 1, 512));
        resolutionZ = static_cast<uint32_t>((std::clamp)(resZ, 1, 512));
        chunkCount  = static_cast<uint32_t>((std::clamp)(chunks, 1, 64));
        extentX = (std::clamp)(extentX, 0.1f, 10000.0f);
        extentZ = (std::clamp)(extentZ, 0.1f, 10000.0f);
        waveAmplitudeScale = (std::max)(waveAmplitudeScale, 0.0f);
        buoyancyDepth      = (std::max)(buoyancyDepth, 0.1f);
        if (resolutionX != oldResolutionX || resolutionZ != oldResolutionZ ||
            chunkCount  != oldChunkCount  || extentX      != oldExtentX      ||
            extentZ     != oldExtentZ) {
            meshDirty = true;
            foamDirty = true;
        }
        if (materialPath != oldMaterialPath) {
            texDirty = true;
            foamDirty = true;
        }
    }
};

} // namespace fbzz::scene
