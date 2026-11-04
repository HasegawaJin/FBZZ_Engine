/// @file    RagdollControls.hpp
/// @brief   ラグドールの部位操作・支持観測・把持の実行時API。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Ragdoll/RagdollProfile.hpp>
#include <Physics/XPBDGrip.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace fbzz::physics {class XPBDSolver;class World;}
namespace fbzz::scene {
class RagdollRig;
struct RagdollBodyTarget {
    RagdollDriveSpace space=RagdollDriveSpace::WORLD;
    math::Vector3 position,velocity,angularVelocity;
    math::Quaternion rotation=math::Quaternion::Identity();
    float positionWeight=1.0f,rotationWeight=1.0f;
};
struct RagdollBodyState {
    bool valid=false,supported=false;
    float mass=0;
    int contacts=0;
    math::Vector3 position,velocity,angularVelocity;
    math::Quaternion rotation=math::Quaternion::Identity();
};
struct RagdollBodyContact {
    bool valid=false,self=false,dynamic=false;
    math::Vector3 point,normal,otherVelocity;
    float penetration=0;
    std::string otherBone;
};
/// @note CapturePointは一定重心高さの倒立振子による近似。捕捉可能性や自立の保証ではない。
struct RagdollBalanceState {
    bool valid=false,hasSupport=false,captureValid=false;
    float mass=0,centerMargin=0,captureMargin=0,gravity=0,height=0;
    int contacts=0;
    math::Vector3 center,velocity,supportPoint,supportNormal,supportVelocity,capturePoint;
};
struct RagdollGrabState {
    physics::XPBDGripStatus status=physics::XPBDGripStatus::NONE;
    math::Vector3 anchor,target;
    float force=0;
};

/// @note 全操作はRig/Worldを所有するスレッド。Reset/再構築で目標・継続力・把持を破棄する。
class RagdollControls {
public:
    FBZZ_ENGINE_API explicit RagdollControls(RagdollRig& rig);
    FBZZ_ENGINE_API ~RagdollControls();
    RagdollControls(const RagdollControls&)=delete;
    RagdollControls& operator=(const RagdollControls&)=delete;
    FBZZ_ENGINE_API void Reset();
    [[nodiscard]] FBZZ_ENGINE_API std::string BodyName(int index) const;
    [[nodiscard]] FBZZ_ENGINE_API RagdollBodyState BodyState(std::string_view bone) const;
    [[nodiscard]] FBZZ_ENGINE_API RagdollBodyContact Contact(std::string_view bone,int index) const;
    [[nodiscard]] FBZZ_ENGINE_API bool BodyDrive(std::string_view bone,RagdollBodyDrive& out) const;
    /// @note 目標位置は重心。位置・回転の混合率は0〜1、速度は同じ参照空間のm/s・rad/s。
    [[nodiscard]] FBZZ_ENGINE_API bool SetTarget(std::string_view bone,const RagdollBodyTarget& target);
    [[nodiscard]] FBZZ_ENGINE_API bool ClearTarget(std::string_view bone);
    FBZZ_ENGINE_API void ClearTargets();
    /// @note point=nullptrは現在の重心。力積はN·s、角力積はN·m·s。失敗時は未変更。
    [[nodiscard]] FBZZ_ENGINE_API bool Impulse(std::string_view bone,const math::Vector3& value,const math::Vector3* point=nullptr);
    [[nodiscard]] FBZZ_ENGINE_API bool AngularImpulse(std::string_view bone,const math::Vector3& value);
    /// @note N・N·mと秒。継続力は残り時間だけ積分し、callback回数やsubstep数で力積を増やさない。
    [[nodiscard]] FBZZ_ENGINE_API bool Force(std::string_view bone,const math::Vector3& value,float seconds,const math::Vector3* point=nullptr);
    [[nodiscard]] FBZZ_ENGINE_API bool Torque(std::string_view bone,const math::Vector3& value,float seconds);
    [[nodiscard]] FBZZ_ENGINE_API RagdollBalanceState Balance(float padding=0.02f,float minSupportDot=0.5f) const;
    [[nodiscard]] FBZZ_ENGINE_API bool Grab(std::string_view bone,physics::RigidBody* other,const math::Vector3& point,const physics::XPBDGripSettings& settings);
    [[nodiscard]] FBZZ_ENGINE_API bool Release(std::string_view bone);
    FBZZ_ENGINE_API void ReleaseAll();
    [[nodiscard]] FBZZ_ENGINE_API RagdollGrabState GrabState(std::string_view bone) const;
    [[nodiscard]] FBZZ_ENGINE_API bool HasGrabs() const;
    /// @note 動的な把持対象との接触は、最後の解除後も全形状が2cm以上離れるまで除外する。静的な支持面は除外しない。
    FBZZ_ENGINE_API void UpdateContactExclusions(physics::World& world);
    [[nodiscard]] FBZZ_ENGINE_API bool IgnoresContactsWith(const physics::RigidBody* body) const;
    /// @note Rigの予測後・接触生成前に継続力を反映する。
    FBZZ_ENGINE_API void IntegrateForces(float h);
    FBZZ_ENGINE_API void RegisterGrabs(physics::XPBDSolver& solver);
    [[nodiscard]] FBZZ_ENGINE_API RagdollDriveSpace Space(std::string_view bone,RagdollDriveSpace fallback) const;
    FBZZ_ENGINE_API void BlendTarget(std::string_view bone,math::Vector3& position,math::Quaternion& rotation,
        math::Vector3& velocity,math::Vector3& angular) const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
