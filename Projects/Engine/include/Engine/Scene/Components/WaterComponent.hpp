// FBZZ Engine
// WaterComponent.hpp | fbzz::scene
// 水面描画に必要な形状・色・波・泡・流れ・波紋設定を保持するコンポーネント
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

// WaterComponent — シーン上の海・湖・川 1 面分の水面データ。
// WHAT: CPU 側は平坦な XZ グリッドを生成し、GPU 側で波変位・法線・泡・屈折を合成する。
struct WaterComponent {
    // ── ジオメトリ ──────────────────────────────────────────────────────────
    uint32_t resolutionX = 64;
    uint32_t resolutionZ = 64;
    float    extentX     = 100.0f;
    float    extentZ     = 100.0f;
    // チャンク分割数。水面を chunkCount×chunkCount のサブメッシュに分割し、
    // チャンク単位でフラスタムカリングする。広大な水域での描画負荷削減に有効。
    // WHY: 水面全体は 1 つの DrawCall だとカリングできないが、チャンク分割すれば
    //      視野外のサブメッシュをスキップできる。TerrainComponent の chunkSize に相当。
    uint32_t chunkCount = 4;

    // ── 水色・透明度 ────────────────────────────────────────────────────────
    math::Vector3 shallowColor = { 0.20f, 0.60f, 0.70f };
    math::Vector3 deepColor    = { 0.00f, 0.10f, 0.30f };
    float shallowDepth = 0.5f;
    float deepDepth    = 5.0f;
    float opacity      = 0.85f;

    // ── Fresnel 反射 ────────────────────────────────────────────────────────
    float reflectivity = 0.5f;
    float fresnelBias  = 0.02f;
    float fresnelPower = 5.0f;

    // ── 法線マップ ──────────────────────────────────────────────────────────
    std::string normalMap1Path;
    std::string normalMap2Path;
    float normalMap1Tiling = 4.0f;
    float normalMap2Tiling = 6.0f;
    float normalStrength   = 1.0f;
    math::Vector2 normalMap1Scroll = {  0.02f, 0.01f };
    math::Vector2 normalMap2Scroll = { -0.01f, 0.02f };

    // ── Gerstner 波 ────────────────────────────────────────────────────────
    std::array<GerstnerWave, 4> waves = {};
    bool enableGerstnerWaves = true;

    // ── 岸辺泡 ──────────────────────────────────────────────────────────────
    float foamThreshold = 0.3f;
    float foamFade      = 0.5f;
    float foamStrength  = 1.0f;
    std::string foamTexPath;
    float foamTiling    = 8.0f;

    // ── スクリーンスペース屈折 ─────────────────────────────────────────────
    float refractionStrength = 0.03f;

    // ── フローマップ ────────────────────────────────────────────────────────
    bool        enableFlowMap = false;
    std::string flowMapPath;
    float       flowSpeed     = 0.3f;
    float       flowTiling    = 1.0f;

    // ── コースティクス用設定（描画パスは後続 Phase で使用） ────────────────
    bool        enableCaustics    = true;
    float       causticsIntensity = 0.4f;
    float       causticsTiling    = 0.5f;
    float       causticsSpeed     = 0.15f;
    std::string causticsTexPath;

    // ── 反射・状態 ──────────────────────────────────────────────────────────
    std::string envCubemapPath;

    // ── アセット外部化 ──────────────────────────────────────────────────────
    // waterAssetPath が空でない場合、このパスの .fbzzwater に視覚パラメータを外部保存する。
    // シーン側には enabled / extentX / extentZ / resolutionX / resolutionZ のみ残る。
    // WHY: TerrainComponent の terrainAssetPath と同じ設計。プリセットの再利用と
    //      シーンファイルの軽量化を両立する。
    std::string waterAssetPath;

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

    // Reflect — Inspector / Serializer から編集・保存するフィールド一覧。
    // WHAT: 配列や高度なアセット保存は専用 Serializer に委譲し、基本パラメータは反射で扱う。
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        // IReflector は uint32_t 非対応のため int 経由で編集・保存する。
        // WHY: Component の公開型はメッシュ生成と一致する unsigned のまま保ち、UI 境界だけ変換する。
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
        r.Field("shallowColor", shallowColor);
        r.Field("deepColor", deepColor);
        r.Field("shallowDepth", shallowDepth);
        r.Field("deepDepth", deepDepth);
        r.Field("opacity", opacity);
        r.Field("reflectivity", reflectivity);
        r.Field("fresnelBias", fresnelBias);
        r.Field("fresnelPower", fresnelPower);
        r.Field("normalMap1Path", normalMap1Path);
        r.Field("normalMap2Path", normalMap2Path);
        r.Field("normalMap1Tiling", normalMap1Tiling);
        r.Field("normalMap2Tiling", normalMap2Tiling);
        r.Field("normalStrength", normalStrength);
        r.Field("normalMap1Scroll", normalMap1Scroll);
        r.Field("normalMap2Scroll", normalMap2Scroll);
        r.Field("enableGerstnerWaves", enableGerstnerWaves);
        r.Field("foamThreshold", foamThreshold);
        r.Field("foamFade", foamFade);
        r.Field("foamStrength", foamStrength);
        r.Field("foamTexPath", foamTexPath);
        r.Field("foamTiling", foamTiling);
        r.Field("refractionStrength", refractionStrength);
        r.Field("enableFlowMap", enableFlowMap);
        r.Field("flowMapPath", flowMapPath);
        r.Field("flowSpeed", flowSpeed);
        r.Field("flowTiling", flowTiling);
        r.Field("enableCaustics", enableCaustics);
        r.Field("causticsIntensity", causticsIntensity);
        r.Field("causticsTiling", causticsTiling);
        r.Field("causticsSpeed", causticsSpeed);
        r.Field("causticsTexPath", causticsTexPath);
        r.Field("envCubemapPath", envCubemapPath);
        r.Field("waterAssetPath", waterAssetPath);
    }
};

} // namespace fbzz::scene
