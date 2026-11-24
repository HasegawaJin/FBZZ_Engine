/// @file    RenderSceneGeometryCache.hpp
/// @brief   抽出スレッド内で共有する静的メッシュの入力値と寿命情報。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Rendering/Extraction/RenderSceneExtractor.hpp>
#include <Engine/Scene/References/Entity.hpp>
#include <array>

namespace fbzz::renderer { struct Mesh; }

namespace fbzz::scene::render_extraction_detail {

/// @note Scene 世代とコンポーネントの寿命を照合し、メッシュは非所有の識別子として保持する。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine キャッシュした入力と資源の寿命。
struct StaticGeometryEntry {
    EntityID entity;
    uint64_t lifetime = 0;
    uint64_t lastExtractFrame = UINT64_MAX, lastPoseChangeFrame = UINT64_MAX;
    const renderer::Mesh* mesh = nullptr;
    std::array<uint32_t, 10> pose{};
    std::array<uint32_t, 14> meshInputs{};
    math::Matrix4 world, inverseTranspose;
    math::Vector3 center{};
    float radius = 0;
    float windPadding = 0, occluderRadius = 0;
    bool reliable = false, valid = false;
};

struct SpatialObjectIdentity {
    EntityID entity;
    uint64_t lifetime = 0;
    bool skinned = false;
    bool operator==(const SpatialObjectIdentity&) const = default;
};

struct StaticSceneGeometry {
    const Scene* scene = nullptr;
    uint64_t generation = 0, revision = 0;
    bool primed = false;
    bool highChangeLastBuild = false;
    uint32_t denseBuildsRemaining = 0;
    std::vector<StaticGeometryEntry> entries;
    uint64_t spatialVersion = 0;
    std::vector<SpatialObjectIdentity> spatialObjects;
};

/// @note マテリアルなどのコールバックをまたいで戻り値を保持しない。再入した抽出がスロットを更新し得る。
[[nodiscard]] StaticSceneGeometry* FindStaticGeometry(Scene& scene, bool create);

}
