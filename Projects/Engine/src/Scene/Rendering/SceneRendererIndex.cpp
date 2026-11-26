/// @file    SceneRendererIndex.cpp
/// @brief   描画候補の登録と所有順を照合し、候補集合を再利用する。
/// @author  Hasegawa Jin
/// @date    2026-11-22
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Geometry/MeshRenderer.hpp>
#include <Engine/Scene/Components/Geometry/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

namespace fbzz::scene {
namespace {

enum class RendererWorksetMode { LEGACY, REVISION, VALIDATE };

RendererWorksetMode RendererWorksetModeForProcess()
{
    static const RendererWorksetMode mode = [] {
        const char* value = std::getenv("FBZZ_RENDERER_WORKSET_MODE");
        const std::string_view requested = value ? value : "";
        const auto selected = requested == "revision" ? RendererWorksetMode::REVISION
            : requested == "validate" ? RendererWorksetMode::VALIDATE : RendererWorksetMode::LEGACY;
        const char* name = selected == RendererWorksetMode::REVISION ? "revision"
            : selected == RendererWorksetMode::VALIDATE ? "validate" : "legacy";
        std::fprintf(stderr, "RENDERER_WORKSET_CONFIG mode=%s\n", name);
        return selected;
    }();
    return mode;
}

struct RendererOrderWitness {
    EntityID m_entity;
    size_t m_ownerIndex = SIZE_MAX;
    bool operator==(const RendererOrderWitness&) const = default;
};

/// @note Scene のアドレスは識別にだけ使う。構造識別値が一致しないときだけ現在の密 ID 列・所有位置を再照合する。
struct RendererEntityIndex {
    const Scene* m_scene = nullptr;
    uint64_t m_generation = 0;
    std::array<std::vector<RendererOrderWitness>, 3> m_sources;
    std::array<std::vector<EntityID>, 3> m_ordered;
    std::array<bool, 3> m_ready{};
    std::array<uint64_t, 3> m_membershipRevisions{};
    std::array<uint64_t, 3> m_ownerRevisions{};
    bool m_revisionRejected = false;
    std::unique_ptr<RendererEntityIndex> m_legacyOracle; ///< validate だけで確保し、構造識別値を使わない旧照合の独立状態を保持する。

    void Reset(const Scene& scene)
    {
        m_scene = &scene;
        m_generation = scene.GetRenderSceneGeneration();
        for (auto& source : m_sources) {
            source.clear();
        }
        for (auto& ordered : m_ordered) {
            ordered.clear();
        }
        m_ready.fill(false);
        m_membershipRevisions.fill(0);
        m_ownerRevisions.fill(0);
        m_revisionRejected = false;
        m_legacyOracle.reset();
    }
};

/// @note ID と所有順だけの sidecar はスレッドごと最大 4 Scene に閉じ、退役した Scene や Component を参照しない。
RendererEntityIndex& RendererIndexFor(const Scene& scene)
{
    static thread_local std::array<RendererEntityIndex, 4> indices;
    static thread_local size_t nextIndex = 0;
    for (auto& index : indices) {
        if (index.m_scene != &scene) {
            continue;
        }
        if (index.m_generation != scene.GetRenderSceneGeneration()) {
            index.Reset(scene);
        }
        return index;
    }
    auto& index = indices[nextIndex];
    nextIndex = (nextIndex + 1) % indices.size();
    index.Reset(scene);
    return index;
}

}

void Scene::CopyRendererEntities(std::vector<EntityID>& output, RendererEntitySet set) const
{
    FBZZ_PROFILE_SCOPE("Scene::RendererCandidates");
    size_t setIndex = 0;
    switch (set) {
    case RendererEntitySet::GEOMETRY: break;
    case RendererEntitySet::SKINNED: setIndex = 1; break;
    case RendererEntitySet::GEOMETRY_AND_TERRAIN: setIndex = 2; break;
    default: output.clear(); return;
    }

    const auto mode = RendererWorksetModeForProcess();
    auto& index = RendererIndexFor(*this);
    const std::array sources{GetEntities<MeshRenderer>(), GetEntities<SkinnedMeshRenderer>(), GetEntities<TerrainComponent>()};
    const std::array revisions{GetArray<MeshRenderer>().GetStructureRevision(),
        GetArray<SkinnedMeshRenderer>().GetStructureRevision(), GetArray<TerrainComponent>().GetStructureRevision()};
    const std::array used{set != RendererEntitySet::SKINNED, true, set == RendererEntitySet::GEOMETRY_AND_TERRAIN};
    bool useRevision = mode != RendererWorksetMode::LEGACY && !index.m_revisionRejected;
    if (useRevision) {
        bool known = m_rendererOwnerRevision != 0;
        for (size_t source = 0; source < used.size(); ++source)
            if (used[source] && !sources[source].empty() && revisions[source] == 0) {
                known = false;
            }
        if (!known) {
            index.m_revisionRejected = true;
            index.m_ready.fill(false);
            useRevision = false;
            std::fprintf(stderr, "RENDERER_WORKSET_FALLBACK reason=unknown_structure_token\n");
        }
    }
    bool refreshed = false;
    const auto ownerIndex = [this](EntityID id) {
        const auto* object = GetGameObject(id);
        return object ? object->m_sceneStorageIndex : SIZE_MAX;
    };
    const auto copyIndex = [&](RendererEntityIndex& target, bool trustRevision, bool profileRebuild) -> const std::vector<EntityID>& {
        const auto refresh = [&](size_t sourceIndex) {
            if (trustRevision && target.m_membershipRevisions[sourceIndex] == revisions[sourceIndex]
                && target.m_ownerRevisions[sourceIndex] == m_rendererOwnerRevision) {
                return false;
            }
            if (trustRevision) {
                refreshed = true;
            }
            const auto entities = sources[sourceIndex];
            auto& previous = target.m_sources[sourceIndex];
            bool changed = previous.size() != entities.size();
            if (!changed) {
                for (size_t position = 0; position < entities.size(); ++position) {
                    if (previous[position].m_entity != entities[position]
                        || previous[position].m_ownerIndex != ownerIndex(entities[position])) {
                        changed = true;
                        break;
                    }
                }
            }
            if (changed) {
                previous.resize(entities.size());
                for (size_t position = 0; position < entities.size(); ++position) {
                    previous[position] = {entities[position], ownerIndex(entities[position])};
                }
            }
            target.m_membershipRevisions[sourceIndex] = revisions[sourceIndex];
            target.m_ownerRevisions[sourceIndex] = m_rendererOwnerRevision;
            return changed;
        };

        // span は callback を呼ばないこの照合中だけ借り、索引には ID と所有位置だけをコピーする。
        if (refresh(1)) {
            target.m_ready.fill(false);
        }
        if (used[0] && refresh(0)) {
            target.m_ready[0] = false;
            target.m_ready[2] = false;
        }
        if (used[2] && refresh(2)) {
            target.m_ready[2] = false;
        }

        auto& ordered = target.m_ordered[setIndex];
        const auto rebuild = [&] {
            ordered.clear();
            const auto append = [&](size_t sourceIndex) {
                for (const auto& witness : target.m_sources[sourceIndex]) {
                    ordered.push_back(witness.m_entity);
                }
            };
            if (used[0]) {
                append(0);
            }
            append(1);
            if (used[2]) {
                append(2);
            }
            SortEntitiesBySceneOrder(ordered);
            ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
            target.m_ready[setIndex] = true;
        };
        if (!target.m_ready[setIndex]) {
            if (profileRebuild) {
                FBZZ_PROFILE_SCOPE("Scene::RendererIndexRebuild");
                rebuild();
            } else {
                rebuild();
            }
        }
        return ordered;
    };

    const auto& ordered = copyIndex(index, useRevision, true);
    if (mode == RendererWorksetMode::VALIDATE && useRevision) {
        if (!index.m_legacyOracle) {
            index.m_legacyOracle = std::make_unique<RendererEntityIndex>();
            index.m_legacyOracle->Reset(*this);
        }
        auto& oracle = *index.m_legacyOracle;
        // oracle は現在値の完全照合後に毎回整列し、候補の ready と構造識別値を信用しない。
        oracle.m_ready[setIndex] = false;
        const auto& expected = copyIndex(oracle, false, false);
        bool matches = ordered == expected;
        for (size_t source = 0; source < used.size(); ++source)
            if (used[source] && index.m_sources[source] != oracle.m_sources[source]) {
                matches = false;
            }
        if (!matches) {
            index.m_revisionRejected = true;
            index.m_ready.fill(false);
            useRevision = false;
            std::fprintf(stderr, "RENDERER_WORKSET_FALLBACK reason=validation_mismatch\n");
            copyIndex(index, false, true);
        }
    }
    FBZZ_PROFILE_MARKER(!useRevision ? "Scene::RendererIndexLegacy"
        : refreshed ? "Scene::RendererIndexRevisionRefresh" : "Scene::RendererIndexRevisionReuse");
    output.assign(ordered.begin(), ordered.end());
}

}
