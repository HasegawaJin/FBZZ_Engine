// FBZZ Engine
// SwordTrailComponent.hpp | sandbox
// 剣の攻撃区間だけ通常 TrailComponent を有効化して剣筋を描画するスクリプト
#pragma once

#include <Engine/Scene/Script.hpp>
#include "GameVocab.hpp"

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class SwordTrailComponent : public Script {
    FBZZ_SCRIPT(SwordTrailComponent)

public:
    FBZZ_GROUP("References")
    // 旧: FBZZ_FIELD(std::string, ownerName, ...) + 手書き ResolveOwner()。
    // 新: 型安全参照。Inspector に GameObject をドラッグ&ドロップでアサインできる。
    //     未アサイン時は親階層から Player/Enemy を自動探索する (ResolveOwner)。
    FBZZ_REF(GameObject, owner, "Owner")

    FBZZ_GROUP("Timing")
    FBZZ_FIELD_RANGE(float, swingStartTime, 0.18f, "", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, swingEndTime,   0.78f, "", 0.0f, 1.0f)

    FBZZ_GROUP("Trail")
    FBZZ_FIELD_RANGE(float, duration,       0.32f,  "", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, sampleInterval, 0.004f, "", 0.001f, 0.1f)
    FBZZ_FIELD_RANGE(float, minVertexDist,  0.0f,   "", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, widthStart,     0.28f,  "", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, widthEnd,       0.03f,  "", 0.0f, 1.0f)
    // WHY: 鋼の剣閃は「白い高輝度コア → わずかに寒色へ抜けて透明」が最もリアルに見える。
    //      Trail.hlsl は tex(既定 1x1 白) * color の ALPHA_BLEND なので、color 側で白を作る。
    FBZZ_FIELD(Vector4, colorStart, Vector4(1.0f, 1.0f, 1.0f, 0.85f),  "")
    FBZZ_FIELD(Vector4, colorEnd,   Vector4(0.82f, 0.90f, 1.0f, 0.0f), "")
    FBZZ_FIELD(int, maxPoints,          64, "")
    FBZZ_FIELD(int, smoothSubdivisions,  2, "")
    FBZZ_FIELD(std::string, materialPath, "", "")
    FBZZ_FIELD(std::string, texturePath,  "", "")

    FBZZ_GROUP("Blood Particle")
    FBZZ_FIELD(Vector3, bloodEmitVelocity, Vector3(0.0f, 0.45f, 0.15f), "")
    FBZZ_FIELD(Vector3, bloodGravity,      Vector3(0.0f, -2.8f, 0.0f),  "")
    FBZZ_FIELD(Vector4, bloodColorStart,   Vector4(0.75f, 0.0f, 0.0f, 0.9f), "")
    FBZZ_FIELD(Vector4, bloodColorEnd,     Vector4(0.18f, 0.0f, 0.0f, 0.0f), "")
    FBZZ_FIELD_RANGE(float, bloodSizeStart,      0.08f,  "", 0.01f, 1.0f)
    FBZZ_FIELD_RANGE(float, bloodSizeEnd,        0.015f, "", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, bloodLifetime,       0.38f,  "", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, bloodVelocitySpread, 2.4f,   "", 0.0f, 8.0f)
    FBZZ_FIELD(int, bloodBurstCount, 28, "")

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

// Reflect() をフィールド宣言から自動生成する (旧 .generated.hpp の置き換え)。
FBZZ_REFLECT(SwordTrailComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
// WHY (inline): 1 スクリプト = 1 ファイル化に伴い、専用 .cpp / _IMPL ガードを廃止した。
//               inline 化することで、このヘッダが複数 TU から include されても
//               ODR 違反にならない (WeaponHitbox 等が本ヘッダを include するため)。
inline void SwordTrailComponent::OnStart()
{
    m_owner = ResolveOwner();
    ConfigureTrail();
    ConfigureBloodEmitter();
    SetTrailActive(false, true);
    particle.Stop(/*clear=*/true);
}

inline void SwordTrailComponent::OnUpdate()
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

inline void SwordTrailComponent::PlayBloodSpray()
{
    ConfigureBloodEmitter();
    particle.Play(/*restart=*/true);
    particle.Burst(std::max(1, bloodBurstCount));
}

inline GameObject* SwordTrailComponent::ResolveOwner()
{
    // 1) Inspector でアサインされた参照を最優先で解決する。
    if (GameObject* assigned = owner.object())
        return assigned;

    // 2) 未アサインなら剣先の親階層を上がって Player / Enemy を探す (従来挙動のフォールバック)。
    GameObject* current = m_gameObject;
    while (current) {
        if (current->CompareTag(Tags::Player) || current->CompareTag(Tags::Enemy) ||
            current->name == Tags::Player || current->name == Tags::Enemy) {
            return current;
        }
        current = current->GetParent();
    }
    return nullptr;
}

inline bool SwordTrailComponent::IsSwingWindow() const
{
    // WHY: 剣を振る通常コンボ State の「振り区間」だけで剣筋を出す。共有語彙ヘルパーに集約。
    return m_owner && IsAttackSwing(animator, m_owner, swingStartTime, swingEndTime);
}

inline void SwordTrailComponent::ConfigureTrail()
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

inline void SwordTrailComponent::ConfigureBloodEmitter()
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

inline void SwordTrailComponent::SetTrailActive(bool active, bool clear)
{
    trail.SetEnabled(active, clear);
}

} // namespace sandbox
