/// @file    EntityRef.hpp
/// @brief   永続GUIDとScene世代付きの一時EntityIDを保持するGameObject参照。
/// @author  Hasegawa Jin
/// @date    2026-06-10
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <cstdint>
#include <string>
#include <utility>

namespace fbzz::scene {

class Scene;
class GameObject;
struct ScriptSceneProxy;

struct EntityRef {
    mutable EntityID id;
    mutable std::string guid;
    mutable std::uint64_t sceneGeneration = 0;

    bool IsValid() const { return id.IsValid(); }
    /// @note 欠損GUIDも設定済みとして扱い、未設定専用のfallbackへ切り替えない。
    bool IsAssigned() const { return !guid.empty() || id.IsValid(); }

    EntityRef() = default;
    /// @note 旧EntityID指定も同じ構築境界を使い、以前のGUIDとScene cacheを引き継がない。
    EntityRef(EntityID value, std::string persistentGuid = {}, std::uint64_t generation = 0)
        : id(value), guid(std::move(persistentGuid)), sceneGeneration(generation)
    {
    }
    /// @note 旧APIとの境界だけで使用し、consumerはResolveで現在Sceneを検証する。
    operator EntityID() const { return id; }
    bool operator==(EntityID other) const { return id == other; }
    friend bool operator==(EntityID lhs, const EntityRef& rhs) { return rhs == lhs; }

    bool operator==(const EntityRef& other) const
    {
        if (!guid.empty() && !other.guid.empty()) return guid == other.guid;
        if (guid.empty() && other.guid.empty()) return id == other.id;
        return id.IsValid() && other.id.IsValid() && id == other.id;
    }

    /// @return 設定済みGUIDが欠損した場合はnullptr。GUIDは消去しない。
    GameObject* Resolve(const ScriptSceneProxy& scene) const;
    /// @note GUIDを持たない旧EntityID指定は同じSceneの実体から一度移行する。
    GameObject* Resolve(Scene& scene) const;
};

} /// @note namespace fbzz::scene
