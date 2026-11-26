/// @file    RigidBodyComponent.hpp
/// @brief   physics::RigidBody を Scene に紐付けるコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note Scene の Transform と physics::World の剛体状態を同期するための橋渡し。
/// @note RigidBodyComponent が RigidBody の唯一の所有者。World には RigidBody* を渡す。
#pragma once
#include <Engine/Reflection/Reflection.hpp>
#include <Physics/World/BodyHandle.hpp>
#include <Physics/Dynamics/RigidBody.hpp>
#include <memory>

namespace fbzz::scene {

class GameObject;

/// @note 質量の決め方。
/// @note 既定 Manual: 既存シーンの剛体はすべて手入力の質量で調整済みで、自動計算を既定に
/// @note すると開いた瞬間に全部の重さが変わる。密度からの算出はオプトインの機能にする。
enum class MassMode {
    Manual = 0,  ///< 設定した質量 [kg] をそのまま使う。
    FromDensity, ///< コライダー体積と PhysicsMaterial の密度から質量 [kg] を算出する。
};

/// @brief Scene の姿勢と剛体の物理・描画用の確定姿勢を結び付ける。
struct RigidBodyComponent {
    std::unique_ptr<physics::RigidBody> rigidBody; ///< 物理 World に非所有参照を渡す、コンポーネント所有の剛体。
    physics::BodyHandle bodyHandle; ///< 物理 World に登録した剛体のハンドル。未登録時は無効。
    bool enabled = true; ///< 有効階層にいる間に剛体を物理 World へ同期するか。
    /// @note 照会は次の固定ステップで確定した姿勢を使う。キネマティック速度や掃引衝突は発生しない。
    bool poseDriven = false; ///< AI が静的剛体の姿勢を Scene の Transform から指定できるか。

    MassMode massMode = MassMode::Manual; ///< 物理ステップで剛体に適用する質量の決定方法。
    float computedMass = 0.0f; ///< FromDensity で求めた質量 [kg]。Inspector の読み取り専用表示に使う。

    /// @note PhysicsSystem が毎ステップ剛体へ同期し、FlowVolume が相対速度に応じた力を求める。
    /// @see Docs/design/flow-field.md §2
    float flowCoupling = 0.0f; ///< 風・水流との結合係数 [1/s]。0 なら流れの力を受けない。

    math::Vector3 lastPhysicsPosition = math::Vector3::ZERO; ///< 最後に同期したワールド位置 [m]。外部からのテレポート検出に使う。
    math::Quaternion lastPhysicsRotation = math::Quaternion::Identity(); ///< 最後に同期したワールド回転。外部からの姿勢変更検出に使う。
    math::Vector3 previousPhysicsPosition = math::Vector3::ZERO; ///< 描画補間の始点となる直前のワールド位置 [m]。
    math::Quaternion previousPhysicsRotation = math::Quaternion::Identity(); ///< 描画補間の始点となる直前のワールド回転。
    bool hasPhysicsPoseHistory = false; ///< 描画補間が参照できる確定姿勢の履歴があるか。
    bool hasPhysicsSyncState = false; ///< 剛体と Scene のワールド姿勢に同期基準が設定済みか。

    RigidBodyComponent() = default;
    ~RigidBodyComponent() = default;
    RigidBodyComponent(const RigidBodyComponent& o)
        : rigidBody(o.rigidBody ? std::make_unique<physics::RigidBody>(*o.rigidBody) : nullptr)
        , bodyHandle{}
        , enabled(o.enabled)
        , poseDriven(o.poseDriven)
        , massMode(o.massMode)
        , computedMass(o.computedMass)
        , flowCoupling(o.flowCoupling)
    {
        if (rigidBody) {
            ResetPhysicsSyncState(rigidBody->GetPosition(), rigidBody->GetRotation());
        }
    }
    RigidBodyComponent& operator=(const RigidBodyComponent& o)
    {
        if (this != &o) {
            rigidBody    = o.rigidBody ? std::make_unique<physics::RigidBody>(*o.rigidBody) : nullptr;
            bodyHandle   = {};
            enabled      = o.enabled;
            poseDriven   = o.poseDriven;
            massMode     = o.massMode;
            computedMass = o.computedMass;
            flowCoupling = o.flowCoupling;
            hasPhysicsSyncState = false;
            if (rigidBody) {
                ResetPhysicsSyncState(rigidBody->GetPosition(), rigidBody->GetRotation());
            }
        }
        return *this;
    }
    RigidBodyComponent(RigidBodyComponent&&)            = default;
    RigidBodyComponent& operator=(RigidBodyComponent&&) = default;

    const char* GetTypeName() const { return "Rigid Body"; }

    /// @brief 生成・複製・テレポート後の物理同期基準を現在姿勢へ揃える。
    /// @param[in] position ワールド位置 [m]。
    /// @param[in] rotation ワールド回転。保存前に正規化する。
    void ResetPhysicsSyncState(const math::Vector3& position,
                               const math::Quaternion& rotation)
    {
        lastPhysicsPosition = position;
        lastPhysicsRotation = rotation.Normalized();

        // 新規登録やテレポート直後は、補間が別の姿勢から始まらないよう両端を揃える。
        previousPhysicsPosition = position;
        previousPhysicsRotation = rotation.Normalized();
        hasPhysicsPoseHistory = true;
        hasPhysicsSyncState = true;
    }

    /// @brief 物理解決後の確定姿勢を、次回のテレポート検出基準として保存する。
    /// @param[in] position 確定したワールド位置 [m]。
    /// @param[in] rotation 確定したワールド回転。保存前に正規化する。
    void CommitPhysicsSyncState(const math::Vector3& position,
                                const math::Quaternion& rotation)
    {
        if (!hasPhysicsSyncState) {
            ResetPhysicsSyncState(position, rotation);
            return;
        }

        // 最新の姿勢で上書きする前に、描画補間用の直前の姿勢を退避する。
        previousPhysicsPosition = lastPhysicsPosition;
        previousPhysicsRotation = lastPhysicsRotation;
        lastPhysicsPosition = position;
        lastPhysicsRotation = rotation.Normalized();
        hasPhysicsPoseHistory = true;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("poseDriven", poseDriven);
        {
            static constexpr const char* kMassModeLabels[] = { "Manual", "From Density" };
            int massModeIndex = static_cast<int>(massMode);
            r.Enum("massMode", massModeIndex, kMassModeLabels);
            massMode = static_cast<MassMode>(massModeIndex);
        }
        // 剛体の有無より前に出す。physics::RigidBody ではなくこのコンポーネントが
        // 持つ値なので、rigidBody がまだ無い経路でも保存・復元できる必要がある。
        r.Field("flowCoupling", flowCoupling);
        if (!rigidBody) {
            return;
        }

        // RigidBody の内部状態は private を含むため、Reflect では一度ローカル値に写し、
        // 編集後に setter 経由で戻す。これにより質量変更時の invMass / inertia 再計算を保つ。
        auto& body = *rigidBody;
        bool isStatic = body.IsStatic();
        float mass = body.GetMass();
        math::Vector3 velocity = body.GetVelocity();
        math::Vector3 angularVelocity = body.GetAngularVelocity();
        physics::AxisLock freezePosition = body.GetFreezePosition();
        physics::AxisLock freezeRotation = body.GetFreezeRotation();

        r.Field("isStatic", isStatic);
        // massMode で出し分けないのは、Reflect() が返すフィールドの並びがリフレクタ実装
        // (Inspector / AI ブリッジ) 共通の契約で、状態によって形が変わると読み手側が
        // 壊れやすいため。FromDensity で編集を止める UI 表現は専用 Inspector 側が担当する。
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

}
