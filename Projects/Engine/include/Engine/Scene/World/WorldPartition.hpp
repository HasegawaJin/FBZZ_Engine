/// @file    WorldPartition.hpp
/// @brief   容量制限付きの非同期セル常駐管理と、近距離・HLOD 表現の一括公開。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/World/WorldPartitionManifest.hpp>
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
    bool retireSourceObjects = false; ///< 分離された Play シーンだけで有効。制作シーンが既存のベイク元を暗黙に破棄することはない。
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
/// @note SceneManager がスケジューラーより前に呼ぶ。最初の有効コンポーネントが所有者になり、複数所有者は拒否する。
FBZZ_ENGINE_API void UpdateWorldPartitionRuntime(Scene& scene, renderer::ResourceManager* resources, bool playing);
/// @note Scene の内部移動処理。個体の所有先を再接続した後に常駐 ID とリースを移す。
FBZZ_ENGINE_API void TransferWorldPartitionRuntime(Scene& source, Scene& destination);

/// @note 公開操作と破棄はメインスレッド専用。ジョブには不変の入力と完了結果の領域を渡し、Scene を参照しない。
/// @pre 呼び出し側が所有するランタイムは Scene の破棄前にリセットまたは切り離す。GetWorldPartitionRuntime は寿命を自動管理する。
class WorldPartitionRuntime {
public:
    using JobExecutor = std::function<void(std::function<void()>)>;
    FBZZ_ENGINE_API explicit WorldPartitionRuntime(JobExecutor executor = {}, asset::AssetStreamer* streamer = nullptr);
    FBZZ_ENGINE_API ~WorldPartitionRuntime();
    WorldPartitionRuntime(const WorldPartitionRuntime&) = delete;
    WorldPartitionRuntime& operator=(const WorldPartitionRuntime&) = delete;
    /// @note 無効な置き換えでは現在公開中のパーティションを維持する。パスは projectRoot/Assets を基準とする。
    [[nodiscard]] FBZZ_ENGINE_API bool Configure(Scene& scene, const WorldPartitionManifest& manifest,
        const std::string& projectRoot, const WorldPartitionSettings& settings, std::string& diagnostic);
    [[nodiscard]] FBZZ_ENGINE_API bool Open(Scene& scene, const std::string& manifestReference,
        const WorldPartitionSettings& settings, std::string& diagnostic);
    FBZZ_ENGINE_API void Update(const math::Vector3& source, renderer::ResourceManager* resources = nullptr);
    /// @note 明示的なセルの上書きは ClearCellOverride、Configure、Reset まで有効。UNLOADED は保留処理を取り消してセルを解放する。
    [[nodiscard]] FBZZ_ENGINE_API bool RequestCell(WorldCellId id, WorldCellRepresentation representation, std::string& diagnostic);
    [[nodiscard]] FBZZ_ENGINE_API bool ClearCellOverride(WorldCellId id);
    FBZZ_ENGINE_API void RetryFailed();
    FBZZ_ENGINE_API void Reset();
    [[nodiscard]] FBZZ_ENGINE_API WorldPartitionStatus Status() const;
    [[nodiscard]] FBZZ_ENGINE_API const WorldPartitionManifest& Manifest() const;
    /// @note Scene の個体が退役した後に呼ぶ。退役済みの Scene は参照も変更もしない。
    FBZZ_ENGINE_API void DetachRetiredScene();
private:
    /// @note friend の再宣言は名前空間スコープの宣言と同じ DLL リンケージを持つ。
    /// @see https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-1-c4273 MSVC DLL linkage rules.
    friend FBZZ_ENGINE_API void UpdateWorldPartitionRuntime(Scene&, renderer::ResourceManager*, bool);
    friend FBZZ_ENGINE_API void TransferWorldPartitionRuntime(Scene&, Scene&);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// @note Scene が所有するサービスは Scene::Clear またはデストラクターで退役する。戻り値は Scene の生存期間内だけ有効。
[[nodiscard]] FBZZ_ENGINE_API WorldPartitionRuntime& GetWorldPartitionRuntime(Scene& scene);
[[nodiscard]] FBZZ_ENGINE_API const WorldPartitionRuntime* FindWorldPartitionRuntime(const Scene& scene);
/// @note 内部の寿命管理用フック。通常は runtime.Reset() で常駐個体をアンロードする。
FBZZ_ENGINE_API void ForgetWorldPartitionRuntime(Scene& scene);
}
