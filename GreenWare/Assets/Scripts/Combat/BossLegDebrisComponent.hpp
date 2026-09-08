/// @file    BossLegDebrisComponent.hpp
/// @brief   もげた脚の «その後»。剛体で転がり、床に落ち着いてから砕けて消える
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// BossRigComponent::BreakLeg が脚 1 本ぶんの分割メッシュ (`E_*_<接尾辞>`) を
/// 静的メッシュとして写した GameObject を作り、剛体と一緒にこれを載せる。
///
/// WHY ラグドールで垂らさず切り離すか (2026-09-07):
///   壊れた脚を本体にぶら下げたまま物理で垂らす (旧 Use Ragdoll / Drag) と、生きている
///   脚のクリップと壊れた脚の物理が付け根で押し合い、«壊れた» ではなく «動きがおかしい»
///   に見えた。脚は «もげて床に落ちる» 物で、本体に残す理由が無い。切り離してしまえば
///   本体の 21 クリップは 1 コマも触らずに済み、もげた脚は普通の剛体として床に転がる。
///
/// WHY 床に残さず砕くか:
///   6m 級の脚が 4 本、闘技場の床に転がったままだと盤面が読めなくなる (走る場所が
///   減り、引力で滑る敵が引っ掛かる)。落ち着いてから数秒で装甲片と煙になって消える ─
///   «もげた» 実感は転がる数秒で足り、その先は盤面の邪魔でしかない。
///
/// WHY 静的メッシュ (MeshRenderer) で写すか:
///   本体の脚は SkinnedMeshRenderer で、骨が Animator に握られている。同じ submesh を
///   `Boss.fbx:N` として静的に描けば、バインド姿勢の脚がそのまま «物» になる。
///   膝の曲がりまでは写せないが、もげた瞬間に伸び切るのは «力が抜けた» として読める。
#pragma once

#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossLegDebrisComponent : public Script {
    FBZZ_SCRIPT(BossLegDebrisComponent)

public:
    FBZZ_GROUP("Debris")
    FBZZ_FIELD_RANGE(float, settleSpeed, 0.35f, "Settle Speed", 0.0f, 3.0f)
    FBZZ_TOOLTIP("これ未満の速さ [m/s] が続いたら «落ち着いた»")
    FBZZ_FIELD_RANGE(float, settleSeconds, 0.4f, "Settle Seconds", 0.0f, 3.0f)
    FBZZ_TOOLTIP("落ち着いたと見なすまで遅いままで居る長さ [秒]")
    FBZZ_FIELD_RANGE(float, restSeconds, 2.6f, "静止", 0.0f, 15.0f)
    FBZZ_TOOLTIP("落ち着いてから砕けるまで [秒]。«そこに落ちている» を見せる時間")
    FBZZ_FIELD_RANGE(float, maxLifetime, 9.0f, "Max Lifetime", 1.0f, 30.0f)
    FBZZ_TOOLTIP("どこかへ転がり続けても、この時間で必ず砕く")
    FBZZ_FIELD_RANGE(float, shatterStrength, 0.75f, "Shatter", 0.0f, 1.0f)
    FBZZ_TOOLTIP("砕けるときの爆発の強さ (VfxManager::PlayImpact の strength)")
    FBZZ_FIELD_READ_ONLY(std::string, debugState, "Flying", "状態")

    /// 生成した側が呼ぶ。脚の大きさ [m] と «どちらへもげたか» を渡す。
    ///
    /// ⚠ ここで剛体へ書いてはいけない。呼ばれるのは `AddScript` の直後で、そのとき
    ///   スクリプトの context はまだ結ばれていない ─ `GameObject::AddScript` は
    ///   `unique_ptr` を積むだけで、`SetContext` は `ScriptSystem` が後から呼ぶ
    ///   (Scene.hpp)。未結線の `scene` から `GetComponent` を引くと空が返り、
    ///   **もげた脚が速度も回転も貰えないまま真下へ落ちる**。値は控えるだけにして、
    ///   context が揃っている `OnStart` で当てる。
    void Launch(const Vector3& velocity, const Vector3& spin, float extent)
    {
        m_extent   = std::max(extent, 0.5f);
        m_velocity = velocity;
        m_spin     = spin;
    }

    void OnStart() override;
    void OnUpdate() override;

private:
    void Shatter();

    float   m_age      = 0.0f;
    float   m_slowFor  = 0.0f;
    float   m_rest     = -1.0f;   ///< 落ち着いてからの経過。負なら まだ
    float   m_extent   = 2.0f;
    bool    m_done     = false;
    /// Launch が控えた «もげた勢い»。OnStart で剛体へ流す。
    Vector3 m_velocity = {};
    Vector3 m_spin     = {};
};

FBZZ_REFLECT(BossLegDebrisComponent)

inline void BossLegDebrisComponent::OnStart()
{
    // Launch が控えた勢いをここで当てる (上の ⚠ を参照)。
    if (auto* rb = scene.GetComponent<RigidBodyComponent>())
        if (rb->rigidBody) {
            rb->rigidBody->SetVelocity(m_velocity);
            rb->rigidBody->SetAngularVelocity(m_spin);
        }
}

inline void BossLegDebrisComponent::OnUpdate()
{
    if (m_done) return;
    const float dt = std::max(Time::deltaTime, 0.0f);
    m_age += dt;

    float speed = 0.0f;
    if (auto* rb = scene.GetComponent<RigidBodyComponent>())
        if (rb->rigidBody) speed = rb->rigidBody->GetVelocity().Length();

    if (m_rest < 0.0f) {
        // 落ち着くまで。跳ねている途中で一瞬遅くなる (頂点) のは数えない。
        m_slowFor  = speed < std::max(settleSpeed, 0.0f) ? m_slowFor + dt : 0.0f;
        debugState = "Flying";
        if (m_slowFor >= std::max(settleSeconds, 0.0f)) {
            m_rest     = 0.0f;
            debugState = "Resting";
        }
    } else {
        m_rest += dt;
    }

    const bool rested  = m_rest >= std::max(restSeconds, 0.0f);
    const bool overdue = m_age  >= std::max(maxLifetime, 0.5f);
    if (rested || overdue) Shatter();
}

inline void BossLegDebrisComponent::Shatter()
{
    m_done     = true;
    debugState = "Shattered";

    const Vector3 at = transform.worldPosition;
    if (auto* vfx = VfxManagerComponent::Instance()) {
        // 装甲片と火花は衝突の爆発 (Armor Chunks を持つ) で足りる。焦げは残さない ─
        // 盤面を汚さないために砕いているので、跡が残っては本末転倒。
        vfx->PlayGroundBlast(at, BladeSide::None, Clamp01(shatterStrength));
        vfx->PlayGroundDust(at, Vector3::UP, 0.8f, std::max(m_extent * 0.5f, 0.6f));
    }
    se::Play(audio, se::kImpactDebris, 0.8f);

    if (GameObject* self = scene.Self()) scene.Destroy(*self);
}

} // namespace sandbox
