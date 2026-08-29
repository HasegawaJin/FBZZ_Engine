/// @file    EntityRef.hpp
/// @brief   EntityID の安全ラッパー — ScriptSceneProxy 経由で GameObject* を 1 ステップで解決する。
/// @author  Hasegawa Jin
/// @date    2026-06-10
///
/// IReflector は EntityID と同じ Inspector UI / シリアライズフローを使う。
#pragma once
#include <Engine/Scene/Entity.hpp>

namespace fbzz::scene {

class Scene;
class GameObject;
struct ScriptSceneProxy;

struct EntityRef {
    EntityID id;

    bool IsValid() const { return id.IsValid(); }

    bool operator==(const EntityRef&) const = default;

    // scene プロキシ経由で GameObject* を解決する。無効 / 未存在なら nullptr。
    GameObject* Resolve(const ScriptSceneProxy& scene) const;
    // Scene 直接参照版 (シリアライザ・Editor から使う)
    GameObject* Resolve(Scene& scene) const;
};

} // namespace fbzz::scene