/// @file    NavMeshBakeSystem.hpp
/// @brief   非同期の NavMesh ベイクを投入し、取得した入力を検証する。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#pragma once
#include <Engine/Api.hpp>

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

/// @brief バックグラウンドの NavMesh ベイク開始前に確定した数値リンク入力。
struct NavMeshBakeOffMeshLinkInput {
    EntityID entity = EntityID::INVALID;
    std::uint64_t inputSignature = 0;
    math::Vector3 startPoint = math::Vector3::ZERO;
    math::Vector3 endPoint = math::Vector3::ZERO;
    bool bidirectional = true;
    float traversalTime = 0.3f;
    int agentTypeMask = -1;
};

/// @brief 現在のサーフェス、地形、モディファイアー、Off-Mesh Link 入力をハッシュする。
/// @param scene ワールド変換を同期済みの Scene。
/// @param surfaceId サーフェスコンポーネントを持つ Entity。
/// @return 非ゼロの署名。サーフェスまたは GameObject がなければ 0。
/// @note 地形の高さデータを走査するため、毎フレームの呼び出しは避ける。
[[nodiscard]] FBZZ_ENGINE_API std::uint64_t HashNavMeshBakeSources(Scene& scene, EntityID surfaceId);

/// @brief NavMesh ベイクジョブの登録、検証、公開を担う。
class NavMeshBakeSystem final : public ISystem {
public:
    /// @brief スケジューラーで使う名前を返す。
    std::string_view Name() const override { return "NavMeshBakeSystem"; }
    /// @brief ベイク要求を収集するフェーズを返す。
    Phase GetPhase() const override { return Phase::PreScript; }
    /// @brief エディターとシミュレーションの両方のシーンで実行する。
    RunMode GetRunMode() const override { return RunMode::Always; }
    /// @brief NavMesh、変換、マーカー、地形、モディファイアーのアクセスを宣言する。
    FBZZ_ENGINE_API ComponentAccess GetAccess() const override;
    /// @brief 準備ができたジョブを完了し、要求されたベイクを投入する。
    FBZZ_ENGINE_API void Update(SystemContext& ctx) override;

    /// @brief サーフェスの Entity 添字に対応するジョブの現在の進捗を返す。
    FBZZ_ENGINE_API float BakeProgress(std::uint32_t surfaceId) const;

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

}
