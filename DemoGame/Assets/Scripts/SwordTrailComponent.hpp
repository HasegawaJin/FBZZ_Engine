// FBZZ Engine
// SwordTrailComponent.hpp | sandbox
// 剣の攻撃区間だけ通常 TrailComponent を有効化して剣筋を描画するスクリプト
#pragma once

#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SwordTrailComponent : public Script {
    FBZZ_SCRIPT(SwordTrailComponent)

public:
    FBZZ_GROUP("References")
    FBZZ_FIELD(std::string, ownerName, "Player", "Owner Name")

    FBZZ_GROUP("Timing")
    FBZZ_FIELD_RANGE(float, swingStartTime, 0.18f, "Swing Start Time", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, swingEndTime, 0.78f, "Swing End Time", 0.0f, 1.0f)

    FBZZ_GROUP("Trail")
    FBZZ_FIELD_RANGE(float, duration, 0.32f, "Duration", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, sampleInterval, 0.004f, "Sample Interval", 0.001f, 0.1f)
    FBZZ_FIELD_RANGE(float, minVertexDist, 0.0f, "Min Vertex Dist", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, widthStart, 0.28f, "Width Start", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, widthEnd, 0.03f, "Width End", 0.0f, 1.0f)
    FBZZ_FIELD(Vector4, colorStart, Vector4(1.0f, 0.95f, 0.75f, 0.75f), "Color Start")
    FBZZ_FIELD(Vector4, colorEnd, Vector4(1.0f, 0.25f, 0.05f, 0.0f), "Color End")
    FBZZ_FIELD(int, maxPoints, 64, "Max Points")
    FBZZ_FIELD(int, smoothSubdivisions, 2, "Smooth Subdivisions")
    FBZZ_FIELD(std::string, materialPath, "", "Material Path")
    FBZZ_FIELD(std::string, texturePath, "", "Texture Path")

    FBZZ_GROUP("Blood Particle")
    FBZZ_FIELD(Vector3, bloodEmitVelocity, Vector3(0.0f, 0.45f, 0.15f), "Blood Emit Velocity")
    FBZZ_FIELD(Vector3, bloodGravity, Vector3(0.0f, -2.8f, 0.0f), "Blood Gravity")
    FBZZ_FIELD(Vector4, bloodColorStart, Vector4(0.75f, 0.0f, 0.0f, 0.9f), "Blood Color Start")
    FBZZ_FIELD(Vector4, bloodColorEnd, Vector4(0.18f, 0.0f, 0.0f, 0.0f), "Blood Color End")
    FBZZ_FIELD_RANGE(float, bloodSizeStart, 0.08f, "Blood Size Start", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, bloodSizeEnd, 0.015f, "Blood Size End", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, bloodLifetime, 0.38f, "Blood Lifetime", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, bloodVelocitySpread, 2.4f, "Blood Velocity Spread", 0.0f, 8.0f)
    FBZZ_FIELD(int, bloodBurstCount, 28, "Blood Burst Count")

    void OnStart() override;
    void OnUpdate() override;
    void PlayBloodSpray();

private:
    [[nodiscard]] GameObject* ResolveOwner();
    [[nodiscard]] bool IsSwingWindow() const;
    void ConfigureTrail();
    void ConfigureBloodEmitter();
    void SetTrailActive(bool active, bool clear);

    GameObject* m_owner = nullptr;
    bool m_wasSwinging = false;
};

} // namespace sandbox

#include "SwordTrailComponent.generated.hpp"

#ifndef SwordTrailComponent_IMPL
#define SwordTrailComponent_IMPL

namespace sandbox {

void SwordTrailComponent::OnStart()
{
    m_owner = ResolveOwner();
    ConfigureTrail();
    ConfigureBloodEmitter();
    SetTrailActive(false, true);
    particle.Stop(/*clear=*/true);
}

void SwordTrailComponent::OnUpdate()
{
    if (!m_owner || !m_owner->IsValid())
        m_owner = ResolveOwner();

    ConfigureTrail();
    const bool swinging = IsSwingWindow();
    if (swinging && !m_wasSwinging)
        SetTrailActive(true, true);
    else if (!swinging && m_wasSwinging)
        SetTrailActive(false, false);

    m_wasSwinging = swinging;
}

void SwordTrailComponent::PlayBloodSpray()
{
    ConfigureBloodEmitter();
    particle.Play(/*restart=*/true);
    particle.Burst(std::max(1, bloodBurstCount));
}

GameObject* SwordTrailComponent::ResolveOwner()
{
    if (!ownerName.empty()) {
        if (auto* namedOwner = scene.Find(ownerName))
            return namedOwner;
    }

    GameObject* current = m_gameObject;
    while (current) {
        if (current->CompareTag("Player") || current->CompareTag("Enemy") ||
            current->name == "Player" || current->name == "Enemy") {
            return current;
        }
        current = current->GetParent();
    }
    return nullptr;
}

bool SwordTrailComponent::IsSwingWindow() const
{
    if (!m_owner) return false;

    const bool isAttacking =
        animator.IsInState(m_owner, "Slash_01") ||
        animator.IsInState(m_owner, "Slash_02") ||
        animator.IsInState(m_owner, "Slash_03") ||
        animator.IsInState(m_owner, "CrouchSlash");
    if (!isAttacking) return false;

    const float t = animator.GetNormalizedTime(m_owner);
    return t >= swingStartTime && t <= swingEndTime;
}

void SwordTrailComponent::ConfigureTrail()
{
    // WHY: 剣筋は Particle ではなく剣先の移動点列から生成するため、短い寿命と高頻度サンプルにする。
    trail.SetDuration(duration);
    trail.SetMaxPoints(maxPoints);
    trail.SetSampling(sampleInterval, minVertexDist);
    trail.SetWidth(widthStart, widthEnd);
    trail.SetWidthEasing(TrailWidthEasing::EaseOut);
    trail.SetColor(colorStart, colorEnd);
    trail.SetCameraFacing();
    trail.SetSmoothSubdivisions(smoothSubdivisions);
    trail.SetMaterial(materialPath);
    trail.SetTexture(texturePath, 1.0f, 0.0f);
    trail.SetUVMode(TrailUVMode::Stretch);
}

void SwordTrailComponent::ConfigureBloodEmitter()
{
    // WHY: 命中した瞬間の剣筋から血が飛ぶように、剣先オブジェクトのローカル空間で細長い発生範囲にする。
    particle.SetEnabled(true);
    particle.SetPlayback(/*loop=*/false, 0.05f, /*clearOnStop=*/false);
    particle.SetEmitPosition(Vector3::ZERO);
    particle.SetEmitVelocity(bloodEmitVelocity);
    particle.SetVelocitySpread(bloodVelocitySpread);
    particle.SetGravity(bloodGravity);
    particle.SetColor(bloodColorStart, bloodColorEnd);
    particle.SetSize(bloodSizeStart, bloodSizeEnd);
    particle.SetLifetime(bloodLifetime);
    particle.SetEmitRate(0.0f);
    particle.SetMaxParticles(220);
    particle.SetBoxShape(Vector3(0.55f, 0.05f, 0.05f));
    particle.SetBlendMode(ParticleBlendMode::Alpha);
    particle.SetSortMode(ParticleSortMode::BackToFront);
    particle.SetSimulationMode(ParticleSimulationMode::Cpu);
    particle.SetVelocityDamping(0.10f);
    particle.SetAngularVelocity(-5.0f, 5.0f);
}

void SwordTrailComponent::SetTrailActive(bool active, bool clear)
{
    trail.SetEnabled(active, clear);
}

} // namespace sandbox
#endif
