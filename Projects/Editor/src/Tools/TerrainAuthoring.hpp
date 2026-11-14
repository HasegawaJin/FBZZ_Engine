/// @file    TerrainAuthoring.hpp
/// @brief   高さ画像の部分貼り付けと標高・斜面による地形の塗り分け。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/Vector2.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene { class GameObject; class Scene; struct WaterSplineSnapshot; }

namespace fbzz::editor {

class OpData;

/// @note 保存後の参照と実行時世代を保持する。geometry=false は塗りだけを失効させ、形状・物理を再構築しない。
void ApplyTerrainAuthoringSnapshot(scene::TerrainComponent& terrain, const scene::TerrainComponent& snapshot, bool geometry);

/// @note 保存時に割り当てる terrainAssetPath と描画・物理の実行時状態を除き、編集内容の競合を判定する。
bool TerrainAuthoringValuesEqual(const scene::TerrainComponent& first, const scene::TerrainComponent& second);

/// @note Grid arrays are validated separately by brush history, which shares their unchanged blocks.
bool TerrainAuthoringMetadataEqual(const scene::TerrainComponent& first, const scene::TerrainComponent& second);
bool TerrainAuthoringHeightsEqual(std::span<const float> first, std::span<const float> second);

enum class TerrainHeightStampBlend { ADD, REPLACE, MAXIMUM, MINIMUM };

struct TerrainHeightStampSettings {
    math::Vector3 centerLocal;
    float width = 20.0f;
    float depth = 20.0f;
    float rotationDegrees = 0.0f;
    float heightScale = 10.0f;
    float heightOffset = 0.0f;
    float strength = 1.0f;
    float edgeFade = 0.1f;
    TerrainHeightStampBlend blend = TerrainHeightStampBlend::ADD;
};

struct TerrainHeightEdit {
    scene::TerrainHeightRegion region;
    std::vector<scene::TerrainHeightOverride> vertices;
};

/// @return 地形寸法・設定が不正なら false。画像の読み込み前と範囲プレビューでも共用する。
bool IsValidTerrainHeightStamp(const scene::TerrainComponent& terrain, const TerrainHeightStampSettings& settings);

/// @return 失敗時は out 未変更。頂点は行優先順の正規化高さ。
bool PlanTerrainHeightStamp(const scene::TerrainComponent& terrain, const scene::TerrainComponent& image,
    const TerrainHeightStampSettings& settings, TerrainHeightEdit& out);

/// @return 不正な設定・配列・非有限値では false、地形と実行時世代は未変更。
/// @note 寸法・高さは地形ローカルの m、回転は +Y 軸の度数。画像上端を -Z、右端を +X に写す。
/// @note image.heightData は線形の正規化値。双線形補間し、矩形の外は保持、内側は strength と edgeFade [0,0.5] で合成する。
/// @note ADD は画像高さを加算、他は heightOffset + image * heightScale を目標高にする。結果は地形の [-maxHeight,+maxHeight] 内へ収める。
/// @see https://speakerdeck.com/cygames/dainamitukunabian-geng-woke-neng-nisurucyllista-game-enginefalseopunwarudoxiang-kepurosiziyarubei-jing-zhi-zuo-turutomiao-hua-ji-neng Cygames Tech Conference 2021、スライド21の画像ブラシ。
/// @see Docs/design/terrain-authoring.md
bool ApplyTerrainHeightStamp(scene::TerrainComponent& terrain, const scene::TerrainComponent& image,
    const TerrainHeightStampSettings& settings);

struct TerrainBiomeSettings {
    int grassLayer = 0;
    int rockLayer = 1;
    int snowLayer = 2;
    float snowHeight = 420.0f;
    float snowFade = 60.0f;
    float rockSlope = 38.0f;
    float slopeFade = 10.0f;
};

/// @return 無効な寸法・非有限値・重複層・非可逆変換では false、地形は未変更。
/// @note 標高は world Y [m]、傾斜は world 上方向からの角度 [度]。高さ場の三層を置換し、穴と追加メッシュは保持する。
/// @note 雪は緩斜面へ積もり、急斜面では標高によらず岩が残る。結果は保存スプラットなので Fiber と共有できる。
/// @see Docs/design/terrain-authoring.md
bool BakeTerrainBiome(scene::TerrainComponent& terrain, const scene::Transform& transform,
    const TerrainBiomeSettings& settings);

struct TerrainRidgeSettings {
    math::Vector3 start;
    math::Vector3 end;
    float height = 45.0f;
    float leftWidth = 75.0f;
    float rightWidth = 45.0f;
    float bend = 0.0f;
    float peakAlong = 0.55f;
};

/// @return False on invalid shape or budget overflow. The heightfield remains unchanged on failure.
/// @note The ridge uses world XZ and signed world metres. Tile borders are preserved.
bool SculptTerrainRidge(scene::TerrainComponent& terrain, const scene::Transform& pose, const TerrainRidgeSettings& settings);

/// @return False leaves out unchanged. The plan uses normalized heights and preserves the tile border.
bool PlanTerrainRidge(const scene::TerrainComponent& terrain, const scene::Transform& pose,
    const TerrainRidgeSettings& settings, TerrainHeightEdit& out);

struct TerrainMassifSettings {
    TerrainRidgeSettings spine;
    int branchesPerSide = 3;
    std::uint32_t seed = 1;
    float erosion = 0.35f;
};

/// @return False on invalid shape or budget overflow. The input heightfield remains unchanged on failure.
bool SculptTerrainMassif(scene::TerrainComponent& terrain, const scene::Transform& pose, const TerrainMassifSettings& settings);

struct TerrainRiverCarveSection {
    math::Vector2 center;
    float waterHeight = 0.0f;
    float halfWidth = 0.0f;
    float depth = 0.0f;
    bool beginsReach = false;
    bool openStart = false;
    bool openEnd = false;
    float groundHeight = 0.0f;
    std::optional<float> sourceLakeHeight;
};

struct TerrainRiverCarveSettings {
    float bankWidth = 8.0f;
    float bankRise = 1.5f;
    float endFade = 8.0f;
    float maxCut = 12.0f;
    int bankLayer = -1;
    int protectedLayer = -1;
    int gentleBankLayer = -1;
    float gentleGrade = 0.08f;
    float depositHeight = 0.0f;
};

struct TerrainRiverCarvePlan {
    using Vertex = scene::TerrainHeightOverride;
    struct Material {
        std::size_t offset;
        std::array<std::uint8_t, scene::TERRAIN_SPLAT_SLOTS> indices;
        std::array<std::uint8_t, scene::TERRAIN_SPLAT_SLOTS> weights;
    };
    std::vector<Vertex> vertices;
    std::vector<Material> materials;
    std::vector<std::size_t> protectedIndices;
    std::size_t protectedVertices = 0;
    float deepestCut = 0.0f;
    float highestDeposit = 0.0f;
    float analysisSpacing = 0.0f;
};

/// @note The section positions and settings are world metres. Proposed heights are absolute normalized terrain samples.
/// @return False for invalid or over-budget geometry. Terrain and output remain unchanged on failure.
bool PlanTerrainRiverCarve(const scene::TerrainComponent& terrain, const scene::Transform& pose,
    std::span<const TerrainRiverCarveSection> sections, const TerrainRiverCarveSettings& settings, TerrainRiverCarvePlan& out,
    bool includeBoundary = false);

struct TerrainShoreSettings {
    math::Vector3 seed;
    float width = 120.0f;
    float depth = 120.0f;
    float spacing = 2.0f;
    float tolerance = 0.5f;
};

struct TerrainShoreOutline {
    std::vector<math::Vector3> outer;
    std::vector<std::vector<math::Vector3>> islands;
};

struct TerrainAnalysisSurface {
    const scene::TerrainComponent* terrain;
    const scene::Transform* pose;
};

/// @return Resolves an explicit unique GUID array, or the single target when the array is omitted.
/// @note Flush world transforms before sampling the returned surfaces.
bool ResolveTerrainAnalysisSurfaces(scene::Scene& world, const scene::GameObject* single, const OpData* tiles,
    std::vector<TerrainAnalysisSurface>& out);

enum class TerrainMapChannel { HEIGHT, SLOPE, CAVITY, FLOW, ROUGHNESS, DENSITY };
inline constexpr std::array TERRAIN_MAP_NAMES{"Height", "Slope", "Cavity", "Flow", "Roughness", "Density"};
inline constexpr int TERRAIN_MAP_PREVIEW_SIZE = 48;

struct TerrainFlowRegion {
    math::Vector3 center;
    float size = 0.0f;
};

struct TerrainHabitatBand {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float fade = 8.0f;
};

struct TerrainHabitatSettings {
    float patchScale = 0.0f;
    float patchMin = 0.0f;
    float patchMax = 1.0f;
    int seed = 1;
    bool avoidWater = false;
    int pathLayer = -1;
    TerrainHabitatBand water;
    TerrainHabitatBand path;
};

struct TerrainMapSettings {
    int resolution = 257;
    float radius = 12.0f;
    float flowFullArea = 250000.0f;
    float density = 1.0f;
    std::array<float, 5> preferences{0.0f, -1.0f, 0.2f, -0.3f, -0.5f};
    TerrainFlowRegion flowRegion;
};

struct TerrainMapRaster {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    /// @pre The raster has at least two samples on each axis and u/v are in [0,1].
    float Sample(float u, float v) const;
};

/// @note The first surface is the output tile. All surfaces share a world XZ drainage domain.
/// @return False on invalid input. The raster remains unchanged.
/// @see Docs/design/terrain-authoring.md
bool BuildTerrainMap(std::span<const TerrainAnalysisSurface> surfaces, const TerrainMapSettings& settings,
    TerrainMapChannel channel, TerrainMapRaster& out, const std::function<float(const math::Vector3&)>& habitat = {});

struct TerrainMaterialRule {
    int layer = -1;
    float weight = 1.0f;
    std::array<float, 5> preferences{};
};

inline constexpr std::array TERRAIN_MATERIAL_ROLES{"Ridge Rock", "Slope Soil", "Valley Litter", "Bank Gravel"};
inline constexpr std::array<TerrainMaterialRule, 4> TERRAIN_NATURAL_MATERIAL_RULES{{
    {0, 1.0f, {0.25f, 0.7f, -1.0f, -0.2f, 0.45f}},
    {1, 0.35f, {0.0f, -0.35f, 0.0f, -0.65f, -0.5f}},
    {2, 0.85f, {-0.1f, -0.8f, 1.0f, -0.65f, -0.6f}},
    {3, 1.4f, {0.0f, -0.65f, 0.3f, 1.0f, -0.4f}},
}};

struct TerrainMaterialBakeSettings {
    std::vector<TerrainMaterialRule> rules;
    std::vector<int> protectedLayers;
    float protectedThreshold = 0.35f;
    float strength = 1.0f;
    const TerrainMapRaster* protection = nullptr;
};

/// @note The first surface must refer to terrain or its unmodified source. All rules are evaluated before modifying splats.
bool BakeTerrainMaterials(scene::TerrainComponent& terrain, std::span<const TerrainAnalysisSurface> surfaces,
    const TerrainMapSettings& maps, const TerrainMaterialBakeSettings& settings);

struct TerrainFlowCarveSettings {
    TerrainFlowRegion region;
    TerrainRiverCarveSettings bed;
    float minArea = 6000.0f;
    float depthScale = 0.65f;
};

/// @note Plans a branched channel network for the first surface using a shared regional/refined drainage graph.
bool PlanTerrainFlowCarve(std::span<const TerrainAnalysisSurface> surfaces, const TerrainFlowCarveSettings& settings,
    TerrainRiverCarvePlan& out, int& reaches, std::string& diagnostic);

struct TerrainFlowNetworkPlan {
    std::vector<TerrainRiverCarvePlan> tiles;
    std::vector<TerrainRiverCarveSection> sections;
    float analysisSpacing = 0.0f;
    int sourceOutletCount = 0;
};

inline constexpr float DEFAULT_WATERFALL_DROP = 3.0f;
inline constexpr float DEFAULT_WATERFALL_GRADE = 0.85f;

/// @return Lake-fed cliff lips with a wet segment on both sides in the same reach.
[[nodiscard]] std::vector<std::size_t> FindTerrainWaterfallLips(std::span<const TerrainRiverCarveSection> sections,
    float minDrop, float minGrade);

/// @note Input surfaces are editable non-overlapping tiles. Layer indices are relative to the first tile.
/// @note Shared heights are planned in world space before conversion back to each tile's height scale.
bool PlanTerrainFlowNetwork(std::span<const TerrainAnalysisSurface> sources, const TerrainFlowCarveSettings& settings,
    TerrainFlowNetworkPlan& out, std::string& diagnostic, std::span<const scene::WaterSplineSnapshot> sourceLakes = {});

/// @pre Mask dimensions and layer index have been validated. strength is in [0,1].
/// @note Vertices touching holes are preserved.
void BlendTerrainMap(scene::TerrainComponent& terrain, const TerrainMapRaster& raster, int layer, float strength);

/// @return Closed world-space contours at seed.y. Failure leaves the output unchanged.
/// @note Only the submerged component connected to seed is traced. Missing surfaces and spills are rejected.
bool TraceTerrainShore(std::span<const TerrainAnalysisSurface> surfaces,
    const TerrainShoreSettings& settings, TerrainShoreOutline& out, std::string& diagnostic);

struct TerrainDrainageNode {
    math::Vector3 ground;
    /// @note Priority-Flood spill elevation is for routing, not the rendered river surface.
    float waterHeight = 0.0f;
    double area = 0.0;
    int downstream = -1;
    /// @note Ground-bounded, non-increasing downstream surface in world metres; requires channel-bed carving.
    float riverHeight = 0.0f;
};

struct TerrainDrainage {
    std::vector<TerrainDrainageNode> nodes;
    int seed = -1;
    int spill = -1;
    int outlet = -1;
    float step = 0.0f;
};

/// @note A small constant lift keeps the authored stream visible over an uncut bed without changing its grade.
inline constexpr float DRAINAGE_RIVER_SURFACE_CLEARANCE = 0.05f;

/// @return The bounded drainage graph, with area zero at samples that cannot reach the search edge.
/// @see https://doi.org/10.1016/j.cageo.2013.04.024 Barnes et al., Priority-Flood.
bool BuildTerrainDrainage(std::span<const TerrainAnalysisSurface> surfaces,
    const TerrainShoreSettings& settings, TerrainDrainage& out, std::string& diagnostic, double minRiverArea = 0.0);

/// @return River width and bed depth in world metres derived from contributing area, independently of spill fill.
math::Vector2 DrainageRiverSize(const TerrainDrainageNode& node, float step);

} /// @note namespace fbzz::editor
