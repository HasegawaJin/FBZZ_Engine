/// @file    ManagedRagdollControls.cpp
/// @brief   ラグドール操作APIの入力検証と値応答。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include "ManagedRagdollControls.hpp"
#include "../ManagedServicesBridge.hpp"
#include "../../Objects/ManagedObjectControls.hpp"
#include <Engine/Scene/Components/Physics/RagdollComponent.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <Engine/Scene/Components/Physics/RigidBodyComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <charconv>
#include <cmath>
namespace fbzz::scene::managed_detail {
namespace {
bool DecodeDriveProfileEntry(std::span<const float> values, RagdollBodyDrivePatch& patch)
{
    const float flags = values[0];
    if (flags < 0.0f || flags > 511.0f || flags != std::floor(flags)) {
        return false;
    }
    const auto mask = static_cast<uint32_t>(flags);
    if ((mask & 1U) != 0) {
        if (values[1] != 0.0f && values[1] != 1.0f) {
            return false;
        }
        patch.enabled = values[1] != 0.0f;
    }
    if ((mask & 2U) != 0) {
        if (values[2] != 0.0f && values[2] != 1.0f) {
            return false;
        }
        patch.space = values[2] == 0.0f ? RagdollDriveSpace::PARENT : RagdollDriveSpace::WORLD;
    }
    constexpr std::optional<float> RagdollBodyDrivePatch::* FIELDS[] = {
        &RagdollBodyDrivePatch::linearFrequency, &RagdollBodyDrivePatch::angularFrequency,
        &RagdollBodyDrivePatch::linearDampingRatio, &RagdollBodyDrivePatch::angularDampingRatio,
        &RagdollBodyDrivePatch::maxForce, &RagdollBodyDrivePatch::maxTorque, &RagdollBodyDrivePatch::targetVelocityMultiplier
    };
    for (std::size_t index = 0; index < std::size(FIELDS); ++index) {
        if ((mask & (1U << (index + 2))) == 0) {
            continue;
        }
        const float value = values[index + 3];
        if (!std::isfinite(value) || value < 0.0f) {
            return false;
        }
        patch.*FIELDS[index] = value;
    }
    return true;
}
} // namespace

ManagedBridgeStatus RunRagdollControls(RagdollComponent& row,GameObject& target,GameObject* related,
    uint32_t selector,int64_t integer,std::span<const float> p,std::span<const std::string> t,ManagedServiceResult& output)
{
    const auto args = [&](size_t floatCount, size_t stringCount = 0, int integerMode = 0, bool allowRelated = false) {
        // 操作ごとの引数の個数と、整数欄に指定できる値域を検証する。
        if (p.size() != floatCount || t.size() != stringCount) {
            return false;
        }
        const bool invalidInteger = integer < 0 || (integerMode == 0 && integer != 0)
            || (integerMode == 1 && integer > 1) || (integerMode == 2 && integer > INT32_MAX);
        if (invalidInteger) {
            return false;
        }

        // 名前は空文字や埋め込み終端を認めず、関連オブジェクトは同じシーンの現在の個体に限る。
        for (const auto& name : t) {
            if (name.empty() || name.find('\0') != std::string::npos) {
                return false;
            }
        }
        return !related || (allowRelated && related->GetScene() == target.GetScene()
            && target.GetScene()->GetGameObject(related->GetID()) == related);
    };
    const auto v3=[&](size_t i=0){return math::Vector3{p[i],p[i+1],p[i+2]};};
    auto* c=row.runtime.rig ? &row.runtime.rig->Controls() : nullptr;
    switch(selector) {
    case 50: {
        if(!args(0,1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto s=c ? c->BodyState(t[0]) : RagdollBodyState{};
        output.integer=(s.valid?1:0)|(s.supported?2:0);
        output.values={s.mass,static_cast<float>(s.contacts),s.position.x,s.position.y,s.position.z,
            s.rotation.x,s.rotation.y,s.rotation.z,s.rotation.w,s.velocity.x,s.velocity.y,s.velocity.z,
            s.angularVelocity.x,s.angularVelocity.y,s.angularVelocity.z};break;
    }
    case 51: {
        if(!args(15,1,1)||p[13]<0||p[13]>1||p[14]<0||p[14]>1) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        RagdollBodyTarget a;
        a.space=integer ? RagdollDriveSpace::WORLD : RagdollDriveSpace::PARENT;
        a.position=v3();a.rotation={p[3],p[4],p[5],p[6]};a.velocity=v3(7);a.angularVelocity=v3(10);
        if(a.rotation.Length()<=math::EPSILON||!std::isfinite(a.rotation.Length())) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        a.positionWeight=p[13];a.rotationWeight=p[14];output.integer=c&&c->SetTarget(t[0],a);break;
    }
    case 52: case 59:
        if(!args(0,1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer=c&&(selector==52 ? c->ClearTarget(t[0]) : c->Release(t[0]));break;
    case 53: case 54: {
        const bool force=selector==54;
        if(!args((integer?6:3)+(force?1:0),1,1)||(force&&p.back()<=0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto point=integer ? v3(3) : math::Vector3::ZERO;
        output.integer=c&&(force ? c->Force(t[0],v3(),p.back(),integer ? &point : nullptr)
            : c->Impulse(t[0],v3(),integer ? &point : nullptr));break;
    }
    case 55: case 64:
        if(!args(selector==55?4:3,1)||(selector==55&&p[3]<=0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.integer=c&&(selector==55 ? c->Torque(t[0],v3(),p[3]) : c->AngularImpulse(t[0],v3()));break;
    case 56: {
        if(!args(2)||p[0]<0||p[1]<0||p[1]>1) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto s=c ? c->Balance(p[0],p[1]) : RagdollBalanceState{};
        output.integer=(s.valid?1:0)|(s.hasSupport?2:0)|(s.captureValid?4:0);
        output.values={s.mass,static_cast<float>(s.contacts),s.centerMargin,s.captureMargin,s.gravity,s.height,
            s.center.x,s.center.y,s.center.z,s.velocity.x,s.velocity.y,s.velocity.z,
            s.supportPoint.x,s.supportPoint.y,s.supportPoint.z,s.supportNormal.x,s.supportNormal.y,s.supportNormal.z,
            s.supportVelocity.x,s.supportVelocity.y,s.supportVelocity.z,s.capturePoint.x,s.capturePoint.y,s.capturePoint.z};break;
    }
    case 57: case 58: {
        const bool body=selector==58;
        if(!args(10,body?2:1,1,body)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        for(size_t i=3;i<p.size();++i)if(p[i]<0)return ManagedBridgeStatus::INVALID_ARGUMENT;
        if(p[3]<=0||p[5]<=0) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        physics::RigidBody* other=nullptr;
        if(body) {
            if(!related||!related->activeInHierarchy()) {
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            }
            uint64_t expected=0,current=0;bool exists=false;
            const auto parsed=std::from_chars(t[1].data(),t[1].data()+t[1].size(),expected);
            if(parsed.ec!=std::errc{}||parsed.ptr!=t[1].data()+t[1].size()||expected==0) {
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            }
            const auto kind=1U;
            const auto status=ManagedObjectControls::Component(*related,kind,ManagedBridgeOp::COMPONENT_READ,exists,&current);
            auto* rb=ManagedObjectControls::IsPendingComponentRemoval(*related,kind) ? nullptr : related->GetComponent<RigidBodyComponent>();
            if(status!=ManagedBridgeStatus::OK||!exists||current!=expected||!rb||!rb->enabled||!rb->rigidBody) {
                return ManagedBridgeStatus::NOT_FOUND;
            }
            other=rb->rigidBody.get();
        }
        physics::XPBDGripSettings s;
        s.frequency=p[3];s.dampingRatio=p[4];s.maxForce=p[5];s.maxTorque=p[6];
        s.breakForce=p[7];s.breakDistance=p[8];s.maxReach=p[9];s.constrainRotation=integer!=0;
        output.integer=c&&row.enabled&&target.activeInHierarchy()&&row.phase!=RagdollPhase::Idle&&c->Grab(t[0],other,v3(),s);break;
    }
    case 60: case 63:
        if(!args(0)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        if(c){if(selector==60)c->ReleaseAll();else c->ClearTargets();}break;
    case 61: {
        if(!args(0,1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto s=c ? c->GrabState(t[0]) : RagdollGrabState{};
        output.integer=static_cast<int64_t>(s.status);
        output.values={s.anchor.x,s.anchor.y,s.anchor.z,s.target.x,s.target.y,s.target.z,s.force};break;
    }
    case 62: {
        if(!args(0,1,2)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto s=c ? c->Contact(t[0],static_cast<int>(integer)) : RagdollBodyContact{};
        output.integer=(s.valid?1:0)|(s.self?2:0)|(s.dynamic?4:0);
        output.values={s.point.x,s.point.y,s.point.z,s.normal.x,s.normal.y,s.normal.z,
            s.otherVelocity.x,s.otherVelocity.y,s.otherVelocity.z,s.penetration};output.text={s.otherBone};break;
    }
    case 65: {
        if(!args(0,1)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        RagdollBodyDrive s;const bool valid=c&&c->BodyDrive(t[0],s);
        output.integer=(valid?1:0)|(s.enabled?2:0)|(s.space==RagdollDriveSpace::WORLD?4:0);
        output.values={s.linearFrequency,s.angularFrequency,s.linearDampingRatio,s.angularDampingRatio,s.maxForce,s.maxTorque,s.targetVelocityMultiplier};break;
    }
    case 66:
        if(!args(0,0,2)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        output.text={c ? c->BodyName(static_cast<int>(integer)) : std::string{}};break;
    case APPLY_RAGDOLL_DRIVE_PROFILE: {
        if (t.size() > RAGDOLL_DRIVE_PROFILE_MAX_BODIES
            || !args(1 + t.size() * RAGDOLL_DRIVE_PROFILE_ENTRY_FLOATS, t.size())
            || !std::isfinite(p[0]) || p[0] < 0.0f) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        std::vector<RagdollBodyDriveEntry> entries;
        entries.reserve(t.size());
        for (std::size_t index = 0; index < t.size(); ++index) {
            auto& entry = entries.emplace_back();
            entry.bone = t[index];
            if (!DecodeDriveProfileEntry(p.subspan(1 + index * RAGDOLL_DRIVE_PROFILE_ENTRY_FLOATS,
                    RAGDOLL_DRIVE_PROFILE_ENTRY_FLOATS), entry.patch)) {
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            }
        }
        output.integer = row.runtime.rig && row.runtime.rig->ApplyDriveProfile(entries, p[0]);
        if (output.integer) {
            row.runtime.driveProfile = std::move(entries);
        }
        break;
    }
    default:return ManagedBridgeStatus::UNSUPPORTED;
    }
    return ManagedBridgeStatus::OK;
}
}
