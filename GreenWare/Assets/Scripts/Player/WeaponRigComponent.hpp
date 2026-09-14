/// @file    WeaponRigComponent.hpp
/// @brief   一本の剣をアニメーション済み武器ソケットへ追従させる。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#pragma once

#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class WeaponRigComponent : public Script {
    FBZZ_SCRIPT(WeaponRigComponent)

public:
    FBZZ_REF(GameObject, sword, "剣")
    FBZZ_FIELD(std::string, gripSocketName, "Attach_Grip", "剣の握り位置")

    [[nodiscard]] bool IsDrawn() const { return m_attached; }
    [[nodiscard]] bool IsBusy() const { return false; }
    void OnStart() override
    {
        GameObject* weapon = sword.Get();
        if (!weapon) weapon = scene.Find(kSwordObject);
        m_attached = weapon != nullptr;
        if (!weapon) {
            debug.LogError("WeaponRigComponent: assign Sword before Play.");
            return;
        }
        auto* attachment = weapon->GetComponent<SocketAttachmentComponent>();
        if (!attachment) attachment = &weapon->AddComponent<SocketAttachmentComponent>();
        attachment->enabled = true;
        attachment->socketName = kSocketWeapon;
        attachment->localSocketName = gripSocketName;
        attachment->blendDuration = 0.0f;
        attachment->followPosition = true;
        attachment->followRotation = true;
        attachment->followScale = false;
    }

private:
    bool m_attached = false;
};

FBZZ_REFLECT(WeaponRigComponent)

} // namespace sandbox
