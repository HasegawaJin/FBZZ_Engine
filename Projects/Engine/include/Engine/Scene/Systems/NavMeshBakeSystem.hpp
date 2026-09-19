/// @file    NavMeshBakeSystem.hpp
/// @brief   NavMeshSurface の needsBake 要求を受けて NavMesh を非同期に再構築するシステム。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include "Engine/Scene/Entity.hpp"
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <unordered_map>

namespace fbzz::scene {

class Scene;

/// surface のベイクソース (自身の設定・対象 Terrain の高さ・寄与する Modifier の配置) を
/// 1 つの値へ畳む。ベイク投入時の値と現在値が違えば、その NavMesh は古い。
/// @return surface か GameObject が無ければ 0。
/// @note Terrain の heightData を全走査するため、毎フレーム呼ばずに間引くこと。
[[nodiscard]] uint64_t HashNavMeshBakeSources(Scene& scene, EntityID surfaceId);

class NavMeshBakeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavMeshBakeSystem"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    void Update(SystemContext& ctx) override;

    /// エディタの進捗バー用（surfaceId は EntityID::index）
    float BakeProgress(uint32_t surfaceId) const;

private:
    struct BakeJob {
        std::future<NavMeshBakeResult>      future;
        std::shared_ptr<std::atomic<float>> progress;
    };
    std::unordered_map<uint32_t, BakeJob> m_jobs;
};

} // namespace fbzz::scene
