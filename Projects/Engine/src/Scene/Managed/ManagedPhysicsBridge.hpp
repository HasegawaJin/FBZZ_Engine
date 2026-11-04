/// @file    ManagedPhysicsBridge.hpp
/// @brief   C# 物理呼び出しの数値検証と非所有ヒットの Scene 対応。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Api.hpp>

#include "Engine/Scene/ManagedBridgeAbi.hpp"
#include "Engine/Scene/Transform.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::physics { class World; }
namespace fbzz::scene {
class GameObject;
class Scene;
struct RigidBodyComponent;

namespace managed_detail {
struct ManagedServiceResult;

/// @note object は呼び出し中だけ有効な非所有参照。永続 GUID と opaque ID の wire 化は Runtime が担当する。
struct ManagedPhysicsHit {
    GameObject* object = nullptr;
    math::Vector3 point = math::Vector3::ZERO;
    math::Vector3 normal = math::Vector3::ZERO;
    float distance = 0.0f;
};

/// @note main thread の Play callback 専用。caller・Scene・Entity 寿命の検証は Runtime が行う。
class ManagedPhysicsBridge {
public:
    /// @return body 不在・無効化は NOT_FOUND。変更は fixedUpdate のみ、失敗時の剛体と Transform は未変更。
    FBZZ_ENGINE_API ManagedBridgeStatus RunBody(GameObject& target, const ManagedBridgeRequest& request,
        ManagedBridgeReply& reply, bool fixedUpdate);

    /// @note 重力とレイヤー行列は読み取りだけ公開する。レイヤーは 0..31 の整数。
    FBZZ_ENGINE_API ManagedBridgeStatus RunWorld(const physics::World& world, const ManagedBridgeRequest& request,
        ManagedBridgeReply& reply);

    /// @return 成功時だけ hits を置き換える。RaycastAll は shape ごと、OverlapSphere は GameObject ごとに返す。
    /// @note 現在の Scene に属する active / enabled Collider だけを World の確定姿勢で検索する。
    FBZZ_ENGINE_API ManagedBridgeStatus Query(Scene& scene, const physics::World& world, const ManagedBridgeRequest& request,
        std::vector<ManagedPhysicsHit>& hits, GameObject* ignored = nullptr);

    /// @note 共通サービス packet から検証済みの値引数を受け取る。変更は FixedUpdate のみ。
    FBZZ_ENGINE_API ManagedBridgeStatus RunCollider(GameObject& target, uint32_t kind, uint32_t property, bool writing,
        std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result, bool fixedUpdate);
    FBZZ_ENGINE_API ManagedBridgeStatus RunRotation(GameObject& target, bool writing, std::span<const float> values,
        ManagedServiceResult& result, bool fixedUpdate);
    FBZZ_ENGINE_API ManagedBridgeStatus RunColliderCommand(GameObject& target, uint32_t kind, uint32_t command,
        std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result, bool fixedUpdate);
    /// @note body を持たない navigation 用 world 移動。body があるときは FixedUpdate の既存 teleport を使う。
    FBZZ_ENGINE_API ManagedBridgeStatus MoveWorld(GameObject& target, const math::Vector3& position, bool fixedUpdate);

private:
    FBZZ_ENGINE_API bool PrepareTeleport(GameObject& target, const math::Vector3& position, const math::Quaternion* rotation = nullptr);
    FBZZ_ENGINE_API Transform* Shadow(GameObject& target);
    FBZZ_ENGINE_API bool Update(GameObject& target);
    FBZZ_ENGINE_API bool Synchronize(GameObject& target);
    FBZZ_ENGINE_API void CommitTransforms();

    std::vector<Transform> m_shadow;
    std::vector<uint32_t> m_epochs;
    std::vector<GameObject*> m_ancestors;
    std::vector<GameObject*> m_queue;
    uint32_t m_epoch = 0;
};

} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
