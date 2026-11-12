/// @file    TerrainScatter.hpp
/// @brief   地形の保存済み形状・層から再現可能なメッシュ配置を生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <Graphics/Effects/RenderInstanceInput.hpp>
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <functional>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene { struct TerrainComponent; struct Transform; }
namespace fbzz::editor {
class OperatorRegistry;
class OpArgs;
struct EditorContext;
struct TerrainMapRaster;

/// @return Failure leaves out unchanged. Preview and placement use the same linear image sampling and local XZ extent.
[[nodiscard]] bool LoadTerrainScatterDensityMap(const std::string& projectRoot, const std::string& reference,
    const scene::TerrainComponent& terrain, TerrainMapRaster& out);

struct TerrainScatterSettings {
    /// @note 地形 X/Z 軸の scale 適用後の格子幅 [m]。個体間の最小距離ではない。
    float spacing = 6.0f;
    float density = 1.0f;
    uint32_t seed = 1;
    float minHeight = -100000.0f, maxHeight = 100000.0f;
    float minSlope = 0.0f, maxSlope = 40.0f;
    int layer = -1;
    float layerThreshold = 0.25f;
    float minScale = 0.8f, maxScale = 1.2f;
    float minYaw = 0.0f, maxYaw = 360.0f;
    bool alignToNormal = false;
    uint32_t maxInstances = 100000;
    bool alignDownslope = false;
    float downslopeJitter = 0.0f;
    float embedDepth = 0.0f;
    float minimumDistance = 0.0f;
    uint32_t maxPlacementsPerCell = 0;
};

inline constexpr size_t TERRAIN_SCATTER_STROKE_LIMIT = 512;
inline constexpr size_t TERRAIN_SCATTER_PALETTE_LIMIT = 16;
inline constexpr float TERRAIN_PAINT_MAX_BRUSH_RADIUS = 1024.0f;

/// @note 円の中心は world XYZ。半径は地形 X/Z 軸の scale 適用後の距離 [m]。
struct TerrainScatterBrush {
    std::span<const math::Vector3> centers;
    float radius = 5.0f;
    bool erase = false;
    bool polygon = false;
    float feather = 0.0f;
};

enum class TerrainPaintReason { ACCEPTED, DENSITY, HOLE, HEIGHT, SLOPE, LAYER, WATER, PATH, OBSTACLE, SPACING, OCCUPIED, CELL_BUDGET, COUNT };
struct TerrainPaintDiagnosticPoint { math::Vector3 position; TerrainPaintReason reason; };
struct TerrainPaintDiagnostics {
    std::array<size_t, static_cast<size_t>(TerrainPaintReason::COUNT)> counts{};
    std::array<size_t, TERRAIN_SCATTER_PALETTE_LIMIT> sources{};
    std::vector<TerrainPaintDiagnosticPoint> points;
};
[[nodiscard]] const char* TerrainPaintReasonLabel(TerrainPaintReason reason);
/// @return 地形接平面で測った領域の被覆 [0,1]。polygon は単純な閉多角形。
[[nodiscard]] float TerrainRegionCoverage(const scene::Transform& terrain, const TerrainScatterBrush& brush, const math::Vector3& world);
[[nodiscard]] bool ValidateTerrainRegion(const scene::Transform& terrain, const TerrainScatterBrush& brush);
/// @pre The terrain has at least 2×2 vertices and a positive cell size. X and Z use terrain-local metres.
[[nodiscard]] float SampleTerrainLayerWeight(const scene::TerrainComponent& terrain, float x, float z, int layer);

struct TerrainPaintPreviewPoint {
    math::Vector3 position;
    size_t sourceIndex;
    renderer::InstancePlacement placement;
    math::Matrix4 world = math::Matrix4::Identity();
    std::string definition;
    bool erase = false;
};

/// @note 確定処理と同じ配置案を読み取り、アセットや履歴を作らずに表示用の候補を返す。
[[nodiscard]] bool BuildTerrainPaintPreview(EditorContext& context, const OpArgs& args,
    std::vector<TerrainPaintPreviewPoint>& points, size_t& total, std::string& reason,
    TerrainPaintDiagnostics* diagnostics = nullptr);

/// @return 失敗時は placements を変更せず reason を設定する。成功時は配置を全置換する。
/// @note 高さは world m、傾斜・yaw は度。density は候補の採用率で、指定層の保存済み重みも乗算する。
/// @note 斜面方向では +Z を下り、+Y を法線に揃える。埋め込みは採用判定後に world m で適用する。
/// @note 穴と追加 surface は配置対象外。target の正の一様 worldScale だけが shear なしの姿勢を保持できる。
/// @note seed と格子から個体 ID を決める。100 万候補・指定個体数を超える処理は部分結果を返さず拒否する。
/// @note 全地形散布の128mセル予算を使う場合、全体の配置上限はセル内選別後に評価する。
/// @note densityMap は対象地形のローカル XZ 全域に対応し、採用率へ乗算する。
/// @see Docs/design/terrain-scatter.md
[[nodiscard]] bool BuildTerrainScatter(const scene::TerrainComponent& terrain,
    const scene::Transform& terrainTransform, const scene::Transform& targetTransform,
    const TerrainScatterSettings& settings, std::vector<renderer::InstancePlacement>& placements,
    std::string& reason, const TerrainScatterBrush* brush = nullptr,
    const std::function<bool(const math::Vector3&)>& accepts = {}, TerrainPaintDiagnostics* diagnostics = nullptr,
    const std::function<TerrainPaintReason(const math::Vector3&)>& classify = {}, const TerrainMapRaster* densityMap = nullptr);

void RegisterTerrainScatterOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
