/// @file    WorldPartition.hpp
/// @brief   Bounded asynchronous cell residency with atomic near and HLOD publication.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/WorldPartitionManifest.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset { class AssetStreamer; }
namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene {
class Scene;

enum class WorldCellRepresentation : uint8_t { UNLOADED, NEAR, HLOD };
enum class WorldCellLoadState : uint8_t { UNLOADED, QUEUED, READING, WAITING_ASSETS, RESIDENT, FAILED };

struct WorldPartitionSettings {
    float nearDistance = 384.0f;
    float hlodDistance = 1536.0f;
    float hysteresis = 64.0f;
    uint32_t maxConcurrentLoads = 2;
    uint32_t maxPublishesPerFrame = 1;
    uint32_t maxResidentCells = 256;
    uint64_t maxCellBytes = 16ull * 1024ull * 1024ull;
    uint64_t maxResidentBytes = 512ull * 1024ull * 1024ull;
    /// @note Enabled only for the isolated Play scene. Authoring scenes never retire their baked sources implicitly.
    bool retireSourceObjects = false;
    bool operator==(const WorldPartitionSettings&) const = default;
};

struct WorldPartitionCellStatus {
    WorldCellId id;
    WorldCellRepresentation desired = WorldCellRepresentation::UNLOADED;
    WorldCellRepresentation resident = WorldCellRepresentation::UNLOADED;
    WorldCellLoadState state = WorldCellLoadState::UNLOADED;
    uint64_t revision = 0;
    uint64_t residentBytes = 0;
    uint32_t objectCount = 0;
    std::string diagnostic;
};

struct WorldPartitionStatus {
    bool configured = false;
    std::string manifestPath;
    std::string diagnostic;
    WorldCellGrid grid;
    WorldPartitionSettings settings;
    uint64_t revision = 0;
    math::Vector3 source{};
    uint32_t loadsInFlight = 0;
    uint32_t residentNear = 0;
    uint32_t residentHlod = 0;
    uint64_t residentBytes = 0;
    uint64_t staleCompletionsDropped = 0;
    std::vector<WorldPartitionCellStatus> cells;
};

[[nodiscard]] FBZZ_ENGINE_API bool ValidateWorldPartitionSettings(const WorldPartitionSettings& settings, std::string& diagnostic);
[[nodiscard]] FBZZ_ENGINE_API const char* WorldCellRepresentationName(WorldCellRepresentation representation);
[[nodiscard]] FBZZ_ENGINE_API const char* WorldCellLoadStateName(WorldCellLoadState state);
/// @note Called by SceneManager before the scheduler. The first enabled component is the partition owner; multiple owners are refused.
FBZZ_ENGINE_API void UpdateWorldPartitionRuntime(Scene& scene, renderer::ResourceManager* resources, bool playing);
/// @note Internal Scene move hook; resident identities and leases move together after object ownership is rebound.
FBZZ_ENGINE_API void TransferWorldPartitionRuntime(Scene& source, Scene& destination);

/// @note All public operations and destruction are main-thread-only. Jobs capture immutable input and completion storage, never Scene pointers.
/// @pre A caller-owned runtime is reset or detached before its Scene is destroyed. GetWorldPartitionRuntime manages this lifetime automatically.
class WorldPartitionRuntime {
public:
    using JobExecutor = std::function<void(std::function<void()>)>;
    FBZZ_ENGINE_API explicit WorldPartitionRuntime(JobExecutor executor = {}, asset::AssetStreamer* streamer = nullptr);
    FBZZ_ENGINE_API ~WorldPartitionRuntime();
    WorldPartitionRuntime(const WorldPartitionRuntime&) = delete;
    WorldPartitionRuntime& operator=(const WorldPartitionRuntime&) = delete;
    /// @note Invalid replacement leaves the currently published partition unchanged. Paths are rooted under projectRoot/Assets.
    [[nodiscard]] FBZZ_ENGINE_API bool Configure(Scene& scene, const WorldPartitionManifest& manifest,
        const std::string& projectRoot, const WorldPartitionSettings& settings, std::string& diagnostic);
    [[nodiscard]] FBZZ_ENGINE_API bool Open(Scene& scene, const std::string& manifestReference,
        const WorldPartitionSettings& settings, std::string& diagnostic);
    FBZZ_ENGINE_API void Update(const math::Vector3& source, renderer::ResourceManager* resources = nullptr);
    /// @note An explicit cell override persists until ClearCellOverride, Configure, or Reset. UNLOADED cancels pending work and unloads that cell.
    [[nodiscard]] FBZZ_ENGINE_API bool RequestCell(WorldCellId id, WorldCellRepresentation representation, std::string& diagnostic);
    [[nodiscard]] FBZZ_ENGINE_API bool ClearCellOverride(WorldCellId id);
    FBZZ_ENGINE_API void RetryFailed();
    FBZZ_ENGINE_API void Reset();
    [[nodiscard]] FBZZ_ENGINE_API WorldPartitionStatus Status() const;
    [[nodiscard]] FBZZ_ENGINE_API const WorldPartitionManifest& Manifest() const;
    /// @note Called after a Scene has retired its objects; does not dereference or mutate the retired Scene.
    FBZZ_ENGINE_API void DetachRetiredScene();
private:
    friend void UpdateWorldPartitionRuntime(Scene&, renderer::ResourceManager*, bool);
    friend void TransferWorldPartitionRuntime(Scene&, Scene&);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @note Scene-owned service lifetime is retired automatically by Scene::Clear/destruction. The returned runtime must not outlive its Scene.
[[nodiscard]] FBZZ_ENGINE_API WorldPartitionRuntime& GetWorldPartitionRuntime(Scene& scene);
[[nodiscard]] FBZZ_ENGINE_API const WorldPartitionRuntime* FindWorldPartitionRuntime(const Scene& scene);
/// @note Internal lifetime hook; callers normally use runtime.Reset() to unload resident objects.
FBZZ_ENGINE_API void ForgetWorldPartitionRuntime(Scene& scene);
} /// @note namespace fbzz::scene
