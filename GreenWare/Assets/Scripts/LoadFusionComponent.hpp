/// @file    LoadFusionComponent.hpp
/// @brief   ロード画面の二極を中央へ収束させ、融合演出を確実に開始する。
/// @author  Hasegawa Jin
/// @date    2026-09-15

#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Title/ElectrodeRig.hpp>
#include <Scripts/Utils/BgmLibrary.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class LoadFusionComponent : public Script {
    FBZZ_SCRIPT(LoadFusionComponent)

public:
    FBZZ_FIELD(float, approachSeconds, 2.4f, "Approach Seconds")
    FBZZ_FIELD(float, fusionRadius, 0.72f, "Fusion Radius")

    void OnStart() override
    {
        bgm::Play(audio, bgm::kLoad, 0.4f);
        ElectrodeRig::ResetFusion();
        m_elapsed = 0.0f;
        m_started = false;
        m_ready = false;

        GameObject* plus = scene.Find("PolarityCore_Plus", true);
        GameObject* minus = scene.Find("PolarityCore_Minus", true);
        if (!plus || !minus) {
            debug.LogWarning("Load fusion requires PolarityCore_Plus and PolarityCore_Minus.");
            return;
        }

        m_center = (plus->transform.position + minus->transform.position) * 0.5f;
        m_startRadius = (plus->transform.position - minus->transform.position).Length() * 0.5f;
        m_ready = true;
        ElectrodeRig::SetFusionApproach(m_center, m_startRadius);
        ElectrodeRig::SetFusionApproach(m_center, m_startRadius);
    }

    void OnUpdate() override
    {
        if (!m_ready || m_started) return;
        if (transition::Active()) return;

        m_elapsed += std::max(time.UnscaledDeltaTime(), 0.0f);
        const float duration = std::max(approachSeconds, 0.1f);
        const float linear = Clamp01(m_elapsed / duration);
        const float eased = linear * linear * linear;
        const float contactRadius = Clamp(fusionRadius * 0.05f, 0.0f, m_startRadius);
        const float radius = m_startRadius + (contactRadius - m_startRadius) * eased;
        ElectrodeRig::SetFusionApproach(m_center, radius);

        if (linear >= 1.0f) {
            m_started = true;
            ElectrodeRig::BeginFusion(m_center);
        }
    }

private:
    Vector3 m_center = Vector3::ZERO;
    float m_startRadius = 0.0f;
    float m_elapsed = 0.0f;
    bool m_ready = false;
    bool m_started = false;
};

FBZZ_REFLECT(LoadFusionComponent)

} // namespace sandbox
