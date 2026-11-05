/// @file    TerrainScatter.hpp
/// @brief   地形の保存済み形状・層から再現可能なメッシュ配置を生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <Graphics/Effects/RenderInstanceInput.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene { struct TerrainComponent; struct Transform; }
namespace fbzz::editor {
class OperatorRegistry;
class OpArgs;
struct EditorContext;

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
};

inline constexpr size_t TERRAIN_SCATTER_STROKE_LIMIT = 512;
inline constexpr size_t TERRAIN_SCATTER_PALETTE_LIMIT = 16;

/// @note 円の中心は world XYZ。半径は地形 X/Z 軸の scale 適用後の距離 [m]。
struct TerrainScatterBrush {
    std::span<const math::Vector3> centers;
    float radius = 5.0f;
    bool erase = false;
};

struct TerrainPaintPreviewPoint {
    math::Vector3 position;
    size_t sourceIndex;
};

/// @note 確定処理と同じ配置案を読み取り、アセットや履歴を作らずに表示用の候補を返す。
[[nodiscard]] bool BuildTerrainPaintPreview(EditorContext& context, const OpArgs& args,
    std::vector<TerrainPaintPreviewPoint>& points, size_t& total, std::string& reason);

/// @return 失敗時は placements を変更せず reason を設定する。成功時は配置を全置換する。
/// @note 高さは world m、傾斜・yaw は度。density は候補の採用率で、指定層の保存済み重みも乗算する。
/// @note 斜面方向では +Z を下り、+Y を法線に揃える。埋め込みは採用判定後に world m で適用する。
/// @note 穴と追加 surface は配置対象外。target の正の一様 worldScale だけが shear なしの姿勢を保持できる。
/// @note seed と格子から個体 ID を決める。100 万候補・指定個体数を超える処理は部分結果を返さず拒否する。
/// @see Docs/design/terrain-scatter.md
[[nodiscard]] bool BuildTerrainScatter(const scene::TerrainComponent& terrain,
    const scene::Transform& terrainTransform, const scene::Transform& targetTransform,
    const TerrainScatterSettings& settings, std::vector<renderer::InstancePlacement>& placements,
    std::string& reason, const TerrainScatterBrush* brush = nullptr);

void RegisterTerrainScatterOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
