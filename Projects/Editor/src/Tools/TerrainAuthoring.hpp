/// @file    TerrainAuthoring.hpp
/// @brief   高さ画像の部分貼り付けと標高・斜面による地形の塗り分け。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/Vector2.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene { class GameObject; class Scene; }

namespace fbzz::editor {

class OpData;

/// @note 保存後の参照と実行時世代を保持する。geometry=false は塗りだけを失効させ、形状・物理を再構築しない。
void ApplyTerrainAuthoringSnapshot(scene::TerrainComponent& terrain, const scene::TerrainComponent& snapshot, bool geometry);

/// @note 保存時に割り当てる terrainAssetPath と描画・物理の実行時状態を除き、編集内容の競合を判定する。
bool TerrainAuthoringValuesEqual(const scene::TerrainComponent& first, const scene::TerrainComponent& second);

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

/// @return 地形寸法・設定が不正なら false。画像の読み込み前と範囲プレビューでも共用する。
bool IsValidTerrainHeightStamp(const scene::TerrainComponent& terrain, const TerrainHeightStampSettings& settings);

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
};

struct TerrainRiverCarveSettings {
    float bankWidth = 8.0f;
    float bankRise = 1.5f;
    float endFade = 8.0f;
    float maxCut = 12.0f;
    int bankLayer = -1;
    int protectedLayer = -1;
};

struct TerrainRiverCarvePlan {
    struct Vertex {
        std::size_t index;
        float normalizedHeight;
    };
    struct Material {
        std::size_t offset;
        std::array<std::uint8_t, scene::TERRAIN_SPLAT_SLOTS> indices;
        std::array<std::uint8_t, scene::TERRAIN_SPLAT_SLOTS> weights;
    };
    std::vector<Vertex> vertices;
    std::vector<Material> materials;
    std::size_t protectedVertices = 0;
    float deepestCut = 0.0f;
};

/// @note The section positions and settings are world metres. Proposed heights are absolute normalized terrain samples.
/// @return False for invalid or over-budget geometry. Terrain and output remain unchanged on failure.
bool PlanTerrainRiverCarve(const scene::TerrainComponent& terrain, const scene::Transform& pose,
    std::span<const TerrainRiverCarveSection> sections, const TerrainRiverCarveSettings& settings, TerrainRiverCarvePlan& out);

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

/// @return Closed world-space contours at seed.y. Failure leaves the output unchanged.
/// @note Only the submerged component connected to seed is traced. Missing surfaces and spills are rejected.
bool TraceTerrainShore(std::span<const TerrainAnalysisSurface> surfaces,
    const TerrainShoreSettings& settings, TerrainShoreOutline& out, std::string& diagnostic);

struct TerrainDrainageNode {
    math::Vector3 ground;
    float waterHeight = 0.0f;
    double area = 0.0;
    int downstream = -1;
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
    const TerrainShoreSettings& settings, TerrainDrainage& out, std::string& diagnostic);

/// @return River width and depth in world metres derived from contributing area and local fill.
math::Vector2 DrainageRiverSize(const TerrainDrainageNode& node, float step);

} /// @note namespace fbzz::editor
