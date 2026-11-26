/// @file    WorldHlod.hpp
/// @brief   制作時の所有者ごとに最も粗いメッシュ LOD を集約し、決定的なセル HLOD プロキシを作る。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Instances/MeshInstancesAsset.hpp>
#include <Engine/Scene/World/WorldCell.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @note span は BuildWorldHlod の呼び出し中だけ不変の制作データを借用する。sourceKey は制作時の所有者を識別する。
struct WorldHlodSource {
    std::string sourceKey;
    math::Matrix4 worldTransform = math::Matrix4::Identity();
    std::span<const asset::InstanceMeshPart> parts;
    std::span<const float> lodHeights;
    std::span<const renderer::InstancePlacement> placements;
    uint32_t layer = 0;
    bool castShadows = true;
    bool includeHlod = true;
    bool hasLocalBounds = false;
    WorldCellBounds localBounds; ///< メッシュのローカル境界は、選択した最も粗い LOD の全パーツと材質の頂点変位を含むこと。
};

struct WorldHlodBuildSettings {
    WorldCellGrid grid;
    uint32_t maxGroups = 65536;
    uint32_t maxInstances = 1048576;
};

struct WorldHlodPlacementSource {
    std::string sourceKey;
    uint32_t sourceId = 0;
};

struct WorldHlodGroup {
    WorldCellId cell;
    math::Vector3 origin;
    uint32_t layer = 0;
    bool castShadows = true;
    WorldCellBounds worldBounds; ///< 寄与する境界が一つでも欠ける場合は hasWorldBounds=false とし、この AABB でグループを棄却しない。
    bool hasWorldBounds = false;
    asset::MeshInstancesAsset proxy;
    std::vector<WorldHlodPlacementSource> sources; ///< sources[i] は i 番目のプロキシ配置を識別する。生成 ID は正かつ一意で、走査順に依存しない。
};

/// @note セルごとに互換性のある所有者を最も粗い LOD の独立したインスタンスアセットへ集約し、LOD の全パーツを保持する。
/// @note 配置スケールは正とする。合成変換は有限で、向きを保ちシアーを含まないアフィン TRS とする。
/// @return 失敗時は false を返し、out を変更せず diagnostic を設定する。入力と参照アセットは読み取り専用。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition---hierarchical-level-of-detail-in-unreal-engine HLOD Layers, Instancing.
[[nodiscard]] FBZZ_ENGINE_API bool BuildWorldHlod(std::span<const WorldHlodSource> sources,
    const WorldHlodBuildSettings& settings, std::vector<WorldHlodGroup>& out, std::string& diagnostic);

}
