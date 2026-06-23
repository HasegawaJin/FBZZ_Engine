// FBZZ Engine
// NavMeshBakeSystem.hpp | fbzz::scene
// NavMeshSurfaceComponent::needsBake が true のときに NavMesh を非同期再構築するシステム。
// Editor の明示的な Bake 操作 / ランタイム要求どちらにも対応する。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"
#include "Engine/Scene/Components/NavMeshSurfaceComponent.hpp"
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <unordered_map>

namespace fbzz::scene {

class NavMeshBakeSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "NavMeshBakeSystem"; }
    Phase            GetPhase()   const override { return Phase::PreScript; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    void Update(SystemContext& ctx) override;

    // エディタの進捗バー用（surfaceId は EntityID::index）
    float BakeProgress(uint32_t surfaceId) const;

private:
    struct BakeJob {
        std::future<NavMesh>                future;
        std::shared_ptr<std::atomic<float>> progress;
    };
    std::unordered_map<uint32_t, BakeJob> m_jobs;
};

} // namespace fbzz::scene
