// FBZZ Engine
// PlayerController.hpp | sandbox
// Sample script for ScriptComponent and Inspector reflection
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>

namespace sandbox {

struct PlayerController : fbzz::scene::Script {
    static constexpr const char* TYPE_NAME = "PlayerController";

    const char* GetTypeName() const override { return TYPE_NAME; }

    float speed = 1.0f;
    bool move = true;

    void Reflect(fbzz::scene::IReflector& reflector) override
    {
        reflector.Field("Speed", speed);
        reflector.Field("Move", move);
    }

    void OnUpdate(float dt) override
    {
        if (!move || !m_gameObject) return;
        m_gameObject->transform.localPosition.x += speed * dt;
        if (m_gameObject->transform.localPosition.x > 4.0f)
            m_gameObject->transform.localPosition.x = -4.0f;
        m_gameObject->transform.position = m_gameObject->transform.localPosition;
    }
};

} // namespace sandbox
