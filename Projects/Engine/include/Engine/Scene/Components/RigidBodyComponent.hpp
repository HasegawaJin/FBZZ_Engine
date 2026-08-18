// FBZZ Engine
// RigidBodyComponent.hpp | fbzz::scene
// physics::RigidBody を Scene に紐付けるコンポーネント
// Scene の Transform と physics::World の剛体状態を同期するための橋渡し。
// RigidBodyComponent が RigidBody の唯一の所有者。World には RigidBody* を渡す。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Physics/BodyHandle.hpp>
#include <Physics/RigidBody.hpp>
#include <memory>

namespace fbzz::scene {

class GameObject;

// 質量の決め方。
// WHY 既定を Manual にするか: 既存シーンの剛体はすべて手入力の質量で調整済みで、
//     自動計算を既定にすると開いた瞬間に全部の重さが変わる。密度からの算出は
//     「そうしたい剛体だけ」が明示的に選ぶ、オプトインの機能にする。
enum class MassMode {
    Manual = 0,     // mass を直接指定する (従来どおり)
    FromDensity,    // コライダー体積 × PhysicsMaterial.density から毎回算出する
};

struct RigidBodyComponent {
    std::unique_ptr<physics::RigidBody> rigidBody;
    physics::BodyHandle bodyHandle;
    bool enabled = true;

    MassMode massMode = MassMode::Manual;
    // FromDensity で算出された質量の記録 (読み取り専用の表示用)。
    // WHY 保持するか: Inspector で「密度からいくつになったか」が見えないと、
    //     数値が妥当かどうかをオーサリング中に判断できない。
    float computedMass = 0.0f;

    // 固定ステップの前後姿勢。ゲームロジック用 Transform とは分離し、描画時だけ補間する。
    // WHY Component が持つか: physics::RigidBody は現在姿勢だけを正として扱い、
    //     描画フレームの都合を Physics 層へ逆流させないため。
    math::Vector3 previousPhysicsPosition = math::Vector3::ZERO;
    math::Vector3 currentPhysicsPosition  = math::Vector3::ZERO;
    math::Quaternion previousPhysicsRotation = math::Quaternion::Identity();
    math::Quaternion currentPhysicsRotation  = math::Quaternion::Identity();
    bool hasPhysicsPoseHistory = false;

    RigidBodyComponent() = default;
    ~RigidBodyComponent() = default;
    RigidBodyComponent(const RigidBodyComponent& o)
        : rigidBody(o.rigidBody ? std::make_unique<physics::RigidBody>(*o.rigidBody) : nullptr)
        , bodyHandle{}
        , enabled(o.enabled)
        , massMode(o.massMode)
        , computedMass(o.computedMass)
    {
        if (rigidBody)
            ResetPhysicsPoseHistory(rigidBody->GetPosition(), rigidBody->GetRotation());
    }
    RigidBodyComponent& operator=(const RigidBodyComponent& o)
    {
        if (this != &o) {
            rigidBody    = o.rigidBody ? std::make_unique<physics::RigidBody>(*o.rigidBody) : nullptr;
            bodyHandle   = {};
            enabled      = o.enabled;
            massMode     = o.massMode;
            computedMass = o.computedMass;
            hasPhysicsPoseHistory = false;
            if (rigidBody)
                ResetPhysicsPoseHistory(rigidBody->GetPosition(), rigidBody->GetRotation());
        }
        return *this;
    }
    RigidBodyComponent(RigidBodyComponent&&)            = default;
    RigidBodyComponent& operator=(RigidBodyComponent&&) = default;

    const char* GetTypeName() const { return "Rigid Body"; }

    // テレポート・生成・複製時は補間区間を潰し、過去位置から尾を引かせない。
    void ResetPhysicsPoseHistory(const math::Vector3& position,
                                 const math::Quaternion& rotation)
    {
        previousPhysicsPosition = position;
        currentPhysicsPosition = position;
        previousPhysicsRotation = rotation.Normalized();
        currentPhysicsRotation = previousPhysicsRotation;
        hasPhysicsPoseHistory = true;
    }

    // 1 fixed step の開始時に現在姿勢を前回姿勢へ送る。
    void BeginPhysicsStep()
    {
        if (!hasPhysicsPoseHistory && rigidBody)
            ResetPhysicsPoseHistory(rigidBody->GetPosition(), rigidBody->GetRotation());
        previousPhysicsPosition = currentPhysicsPosition;
        previousPhysicsRotation = currentPhysicsRotation;
    }

    // 物理解決後の確定姿勢を、補間区間の終点として保存する。
    void CommitPhysicsPose(const math::Vector3& position,
                           const math::Quaternion& rotation)
    {
        if (!hasPhysicsPoseHistory) {
            ResetPhysicsPoseHistory(position, rotation);
            return;
        }
        currentPhysicsPosition = position;
        currentPhysicsRotation = rotation.Normalized();
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        {
            static constexpr const char* kMassModeLabels[] = { "Manual", "From Density" };
            int massModeIndex = static_cast<int>(massMode);
            r.Enum("massMode", massModeIndex, kMassModeLabels);
            massMode = static_cast<MassMode>(massModeIndex);
        }
        if (!rigidBody)
            return;

        // WHY: RigidBody の内部状態は private を含むため、Reflect では一度ローカル値に写し、
        //      編集後に setter 経由で戻す。これにより質量変更時の invMass / inertia 再計算を保つ。
        auto& body = *rigidBody;
        bool isStatic = body.IsStatic();
        float mass = body.GetMass();
        math::Vector3 velocity = body.GetVelocity();
        math::Vector3 angularVelocity = body.GetAngularVelocity();
        physics::AxisLock freezePosition = body.GetFreezePosition();
        physics::AxisLock freezeRotation = body.GetFreezeRotation();

        r.Field("isStatic", isStatic);
        // WHY massMode で出し分けないか: Reflect() が返すフィールドの並びは
        //     リフレクタ実装 (Inspector / AI ブリッジ) 共通の契約で、状態によって形が
        //     変わると読み手側が壊れやすい。FromDensity で編集を止める UI 表現は、
        //     実際に人が触る RigidBody の専用 Inspector 側が担当する。
        r.Field("mass", mass);
        r.Field("velocity", velocity);
        r.Field("angularVelocity", angularVelocity);
        r.Field("freezePositionX", freezePosition.x);
        r.Field("freezePositionY", freezePosition.y);
        r.Field("freezePositionZ", freezePosition.z);
        r.Field("freezeRotationX", freezeRotation.x);
        r.Field("freezeRotationY", freezeRotation.y);
        r.Field("freezeRotationZ", freezeRotation.z);
        r.Field("useGravity", body.m_useGravity);
        r.Field("gravityScale", body.m_gravityScale);
        r.Field("linearDrag", body.m_linearDrag);
        r.Field("angularDrag", body.m_angularDrag);
        r.Field("allowSleeping", body.m_allowSleeping);
        r.Field("useCCD", body.m_useCCD);
        r.Field("ccdRadius", body.m_ccdRadius);
        r.Field("charge", body.m_charge);
        r.Field("isGravitationalSource", body.m_isGravitationalSource);
        r.Field("gravitationalMass", body.m_gravitationalMass);

        body.m_isStatic = isStatic;
        body.SetMass(mass);
        body.SetVelocity(velocity);
        body.SetAngularVelocity(angularVelocity);
        body.SetFreezePosition(freezePosition);
        body.SetFreezeRotation(freezeRotation);
    }
};

} // namespace fbzz::scene
