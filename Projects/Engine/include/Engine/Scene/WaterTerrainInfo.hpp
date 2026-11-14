/// @file    WaterTerrainInfo.hpp
/// @brief   水域の地形底・岸泡と更新署名をワールド座標で共有する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {
class Scene;
struct TerrainComponent;
struct WaterComponent;

inline constexpr uint32_t MAX_FINITE_WATER_TERRAIN_RESOLUTION = 257u;
inline constexpr uint32_t MAX_HORIZON_WATER_TERRAIN_RESOLUTION = 513u;

struct WaterTerrainSource {
    const TerrainComponent* terrain = nullptr;
    Transform transform;
    math::Vector3 boundsMin, boundsMax;
    uint64_t lifetime = 0;
};
struct WaterTerrainInfoInputs {
    std::vector<WaterTerrainSource> terrains;
    uint64_t signature = 0;
};

/// @note Scene 抽出側が物理描画 frame に一度 Collect し、全 view と水域へ渡す。
/// @note 内容検証は新しい surfaceRevision/geometry に一度だけ走査。毎 query/view/frame で height 配列を hash しない。
class WaterTerrainInfoCache {
public:
    /// @return 不正な有効 Terrain 入力では false、out 未変更。未初期化/無効 Terrain は除く。
    /// @note inputs は Scene の Terrain を借用する。Collect〜query/bake の間は Scene と Terrain を編集・破棄しない。
    FBZZ_ENGINE_API bool TryCollect(const Scene& scene, WaterTerrainInfoInputs& out);
private:
    struct Validation {
        uint64_t signature = 0;
        uint64_t revision = 0, layout = 0;
        bool valid = false;
        bool conservative = false;
        float minHeight = 0.0f, maxHeight = 0.0f;
    };
    uint64_t m_sceneGeneration = 0;
    std::unordered_map<uint64_t, Validation> m_validations;
};

struct WaterTerrainSample {
    bool hasBottom = false;
    float bottomY = 0.0f; ///< @note hasBottom のときだけ有効な最上 Terrain 交点の world Y [m]。
    double signedDepth = 0.0; ///< @note 基準水位−bottomY [m]。底が水位以上なら 0 以下。noBottom は depthLimit。
    float depth = 0.0f; ///< @note signedDepth を [0,depthLimit] へ制限した鉛直水深 [m]。
    bool dry = false;
    float shoreFoam = 0.0f;
};
struct WaterTerrainBakeSettings {
    uint32_t width = MAX_FINITE_WATER_TERRAIN_RESOLUTION, height = MAX_FINITE_WATER_TERRAIN_RESOLUTION;
    float foamThreshold = 0.3f, foamFade = 0.5f;
    /// @note 0 keeps buoyancyDepth as the encoding range; positive values override it in world metres.
    float depthRange = 0.0f;
};
struct WaterTerrainInfoAtlas {
    uint32_t width = 0, height = 0;
    float depthRange = 0.0f;
    uint64_t signature = 0;
    /// @note RGBA8: R=岸泡、GB=符号付き水深16bit（G上位/B下位）、A bit0=底あり/bit1=dry/bit2=水深範囲外。
    std::vector<uint8_t> rgba;
    /// @note xy=中心、zw=寸法。水域原点から符号付き Water 軸に沿った world m。0 寸法は保存済み矩形。
    math::Vector4 domain{};
};

/// @note Water instance owns one cache. A missed terrain revision or changed atlas layout requires a full bake.
class WaterTerrainInfoAtlasCache {
public:
    /// @return Same atlas as a full bake. On failure the cache and out remain unchanged.
    FBZZ_ENGINE_API bool TryBake(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
        const Transform& transform, const WaterTerrainBakeSettings& settings, WaterTerrainInfoAtlas& out,
        WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);
    /// @return The immutable cached atlas. On failure the cache and out remain unchanged.
    FBZZ_ENGINE_API bool TryBakeSnapshot(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
        const Transform& transform, const WaterTerrainBakeSettings& settings,
        std::shared_ptr<const WaterTerrainInfoAtlas>& out, WaterSurfaceReadView view);
private:
    struct SourceVersion {
        uint64_t layout = 0, signature = 0, revision = 0;
    };
    uint64_t m_layout = 0;
    std::vector<SourceVersion> m_sources;
    std::shared_ptr<const WaterTerrainInfoAtlas> m_atlas;
};

/// @note Horizon の解像度は局所波紋の Extent から独立させる。
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 ResolveWaterTerrainBakeResolution(const WaterTerrainInfoInputs& inputs,
    const WaterComponent& water, const Transform& transform, WaterSurfaceReadView view);

/// @return 不正入力/結果では false、out 未変更。holes/範囲外を含め底が無い場合は成功し hasBottom=false。
/// @note 全 Terrain の最上交点を選び、水位より上の Terrain も dry として残す。深度-buffer の視線距離とは別の鉛直地形水深。
FBZZ_ENGINE_API bool TrySampleWaterTerrainInfo(const WaterTerrainInfoInputs& inputs, math::Vector2 worldXZ,
    float meanWaterY, float depthLimit, float foamThreshold, float foamFade, WaterTerrainSample& out);

/// @return 有限で有効な water/settings の更新署名。失敗時は false、out 未変更。
/// @note Terrain 内容・water world TRS/extent/depthLimit・foam 設定・解像度を含み、camera/波の時刻を含めない。
FBZZ_ENGINE_API bool TryComputeWaterTerrainInfoSignature(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, uint64_t& out,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return texel 中心を world へ写した RGBA8 atlas。失敗時は false、out 未変更。
/// @note 波を乗せる前の基準水位で焼く。浅瀬による波高の変更は行わず、GPU/CPU の波の正本を変えない。
FBZZ_ENGINE_API bool TryBakeWaterTerrainInfo(const WaterTerrainInfoInputs& inputs, const WaterComponent& water,
    const Transform& transform, const WaterTerrainBakeSettings& settings, WaterTerrainInfoAtlas& out,
    WaterSurfaceReadView view = WaterSurfaceReadView::LIVE_QUERY);

/// @return GB を [-depthRange,+depthRange] [m] へ復元する。無効 range/結果では false、out 未変更。
/// @note nearest の16bit丸め誤差は depthRange/65535 以下。GPU は各 texel を Load 復号し、底ありかつ範囲内の重みだけ補間する。
FBZZ_ENGINE_API bool TryDecodeWaterTerrainDepth(uint8_t highByte, uint8_t lowByte, float depthRange, float& out);
} /// @note namespace fbzz::scene
