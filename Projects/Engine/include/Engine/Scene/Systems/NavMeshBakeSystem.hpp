/// @file    NavMeshBakeSystem.hpp
/// @brief   Submit asynchronous NavMesh bakes and validate their captured inputs.
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Math/Vector3.hpp>
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

class Scene;

/// @brief Numeric link input captured before a background NavMesh bake starts.
struct NavMeshBakeOffMeshLinkInput {
    EntityID entity = EntityID::INVALID;
    std::uint64_t inputSignature = 0;
    math::Vector3 startPoint = math::Vector3::ZERO;
    math::Vector3 endPoint = math::Vector3::ZERO;
    bool bidirectional = true;
    float traversalTime = 0.3f;
    int agentTypeMask = -1;
};

/// @brief Hash the current surface, terrain, modifier, and Off-Mesh Link inputs.
/// @param scene Scene whose world transforms have already been synchronized.
/// @param surfaceId Entity carrying the surface component.
/// @return Nonzero signature, or zero if the surface or its GameObject is missing.
/// @note Terrain height data is scanned; avoid calling this once per frame.
[[nodiscard]] std::uint64_t HashNavMeshBakeSources(Scene& scene, EntityID surfaceId);

/// @brief Schedule, validate, and publish NavMesh bake jobs.
class NavMeshBakeSystem final : public ISystem {
public:
    /// @brief Return the scheduler name.
    std::string_view Name() const override { return "NavMeshBakeSystem"; }
    /// @brief Return the phase in which bake requests are collected.
    Phase GetPhase() const override { return Phase::PreScript; }
    /// @brief Run in editor and simulation scenes.
    RunMode GetRunMode() const override { return RunMode::Always; }
    /// @brief Declare NavMesh, transform, marker, terrain, and modifier access.
    ComponentAccess GetAccess() const override;
    /// @brief Complete ready jobs and submit requested bakes.
    void Update(SystemContext& ctx) override;

    /// @brief Return current job progress for a surface entity index.
    float BakeProgress(std::uint32_t surfaceId) const;

private:
    struct BakeJob {
        EntityID surfaceEntity = EntityID::INVALID;
        std::uint64_t sceneGeneration = 0;
        std::uint64_t surfaceComponentLifetime = 0;
        std::uint64_t sourceSignature = 0;
        std::vector<NavMeshBakeOffMeshLinkInput> offMeshLinks;
        std::future<NavMeshBakeResult> future;
        std::shared_ptr<std::atomic<float>> progress;
    };

    std::unordered_map<std::uint32_t, BakeJob> m_jobs;
};

} /// @note namespace fbzz::scene
