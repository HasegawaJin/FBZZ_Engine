/// @file    ManagedObjectLifecycle.hpp
/// @brief   managed の生成・Prefab 復元・遅延破棄を既存 Scene の操作へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace fbzz::scene {
class GameObject;
class Scene;

namespace managed_detail {

/// @pre Runtime が main thread・Play・現在の live callback と caller の Scene 寿命を検証済み。
class ManagedObjectLifecycle final {
public:
    struct SpawnPose { math::Vector3 position; math::Quaternion rotation; };
    /// @note 全 pose と子孫を含む総容量を検証し、一件でも失敗すると今回の生成を全て戻す。
    FBZZ_ENGINE_API static ManagedBridgeStatus SpawnMany(Scene& scene, std::string_view reference, bool prefab,
        std::span<const SpawnPose> poses, std::vector<GameObject*>& created);
    /// @note pose は両方 null または両方有効。失敗時の created と既存 Scene は未変更。
    FBZZ_ENGINE_API static ManagedBridgeStatus Create(Scene& scene, std::string_view name, GameObject*& created,
        const math::Vector3* position = nullptr, const math::Quaternion* rotation = nullptr);
    /// @note 単一 root の Prefab だけを受け付け、GUID remap・参照復元は既存の生成経路へ委任する。
    /// @note Awake / Start は scheduler が次回の Script snapshot から駆動する。復元 callback は既存 serializer の契約に従う。
    FBZZ_ENGINE_API static ManagedBridgeStatus Instantiate(Scene& scene, std::string_view reference, GameObject*& created,
        const math::Vector3* position = nullptr, const math::Quaternion* rotation = nullptr);
    /// @note delay は有限の秒数かつ非負。受付後も既存 Cleanup が削除するまで対象は有効。
    FBZZ_ENGINE_API static ManagedBridgeStatus Destroy(Scene& scene, GameObject& target, float delay);
    /// @note 全体失敗は queue 未変更で counts は 0。null と重複は skipped、unique 対象の入力順を維持する。
    FBZZ_ENGINE_API static ManagedBridgeStatus DestroyMany(Scene& scene, std::span<GameObject* const> targets,
        float delay, uint32_t& applied, uint32_t& skipped, bool publish = true);
};

} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
