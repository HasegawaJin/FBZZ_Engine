// FBZZ Engine
// WaterComponent.hpp | fbzz::scene
// 水面描画コンポーネント（ジオメトリ・Gerstner 波・マテリアル参照を管理）
//
// 視覚パラメータ（色・テクスチャ・Fresnel・泡・フローマップ等）は
// materialPath が指す .mat ファイルで定義する。
// WHY: パラメータをコンポーネントに持つと再利用・プリセット管理が難しい。
//      fzmat に分離することで Inspector なしにシェーダーごとパラメータを差し替えられる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <array>
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

// WaterComponent — シーン上の水面 1 面分のジオメトリ・波・マテリアル参照。
struct WaterComponent {
    // ── ジオメトリ ──────────────────────────────────────────────────────────
    uint32_t resolutionX = 64;
    uint32_t resolutionZ = 64;
    float    extentX     = 100.0f;
    float    extentZ     = 100.0f;
    // チャンク分割数。水面を chunkCount×chunkCount のサブメッシュに分割し、
    // チャンク単位でフラスタムカリングする。
    uint32_t chunkCount = 4;

    // ── Gerstner 波 ────────────────────────────────────────────────────────
    std::array<GerstnerWave, 4> waves = {};
    bool enableGerstnerWaves = true;

    // ── マテリアルアセット参照 ──────────────────────────────────────────────
    // 色・テクスチャ・Fresnel・泡・フローマップ等の視覚パラメータはここが指す .mat で定義する。
    std::string materialPath;

    // ── 状態フラグ ──────────────────────────────────────────────────────────
    bool enabled   = true;
    bool meshDirty = true;
    bool foamDirty = true;
    bool texDirty  = true;

    const char* GetTypeName() const { return "Water"; }

    // GetSurfaceHeightAt — CPU 側で Gerstner 波の高さだけを評価する。
    // WHY: 浮力やスクリプトが GPU と同じ水面高さを参照できる入口を Component に置く。
    float GetSurfaceHeightAt(float localX, float localZ, float time) const
    {
        if (!enableGerstnerWaves) return 0.0f;

        float height = 0.0f;
        for (const GerstnerWave& wave : waves) {
            if (wave.amplitude < 0.0001f || wave.wavelength <= 0.0001f) continue;
            const math::Vector2 dir = wave.direction.Normalized();
            const float k = math::TWO_PI / wave.wavelength;
            const float omega = std::sqrt(9.8f * k);
            const float phase = k * (dir.x * localX + dir.y * localZ) - omega * time;
            height += wave.amplitude * std::sin(phase);
        }
        return height;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        // IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        int resX = static_cast<int>(resolutionX);
        int resZ = static_cast<int>(resolutionZ);
        int chunks = static_cast<int>(chunkCount);
        r.Field("resolutionX", resX);
        r.Field("resolutionZ", resZ);
        r.Field("chunkCount", chunks);
        resolutionX = static_cast<uint32_t>(resX < 1 ? 1 : resX);
        resolutionZ = static_cast<uint32_t>(resZ < 1 ? 1 : resZ);
        chunkCount  = static_cast<uint32_t>(chunks < 1 ? 1 : chunks);
        r.Field("extentX", extentX);
        r.Field("extentZ", extentZ);
        r.Field("enableGerstnerWaves", enableGerstnerWaves);
        r.Field("materialPath", materialPath);

        // GerstnerWave × 4 を Inspector から編集できるよう公開する
        // IReflector::Field は const char* を要求するため文字列リテラルで渡す
        static constexpr const char* kWaveFieldNames[4][5] = {
            { "wave0_dirX", "wave0_dirZ", "wave0_amplitude", "wave0_wavelength", "wave0_steepness" },
            { "wave1_dirX", "wave1_dirZ", "wave1_amplitude", "wave1_wavelength", "wave1_steepness" },
            { "wave2_dirX", "wave2_dirZ", "wave2_amplitude", "wave2_wavelength", "wave2_steepness" },
            { "wave3_dirX", "wave3_dirZ", "wave3_amplitude", "wave3_wavelength", "wave3_steepness" },
        };
        for (int i = 0; i < 4; ++i) {
            GerstnerWave& w = waves[static_cast<size_t>(i)];
            r.Field(kWaveFieldNames[i][0], w.direction.x);
            r.Field(kWaveFieldNames[i][1], w.direction.y);
            r.Field(kWaveFieldNames[i][2], w.amplitude);
            r.Field(kWaveFieldNames[i][3], w.wavelength);
            r.Field(kWaveFieldNames[i][4], w.steepness);
            // steepness は [0,1] にクランプして反映する（括弧で Windows min/max マクロを回避）
            if (w.steepness < 0.0f) w.steepness = 0.0f;
            if (w.steepness > 1.0f) w.steepness = 1.0f;
        }
    }
};

} // namespace fbzz::scene
