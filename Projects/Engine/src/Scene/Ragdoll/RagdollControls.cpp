/// @file    RagdollControls.cpp
/// @brief   ラグドールの部位操作・支持観測・把持。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <Physics/World/World.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {
namespace {
constexpr std::size_t MAX_PENDING_FORCES = 1024;

bool Finite(const math::Vector3& p) {return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool Finite(const math::Quaternion& q) {return std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::isfinite(q.w);}
struct Point {float x,y;};
float Cross(Point a,Point b,Point c) {return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
float Margin(std::vector<Point> points,Point p,float padding)
{
    std::sort(points.begin(),points.end(),[](Point a,Point b){return a.x<b.x || (a.x==b.x&&a.y<b.y);});
    points.erase(std::unique(points.begin(),points.end(),[](Point a,Point b){return a.x==b.x&&a.y==b.y;}),points.end());
    if (points.empty()) {
        return 0;
    }
    std::vector<Point> hull;
    for (auto v:points) {while(hull.size()>1&&Cross(hull[hull.size()-2],hull.back(),v)<=0) hull.pop_back(); hull.push_back(v);}
    const auto lower=hull.size();
    for (std::size_t i=points.size()-1;i>0;--i) {
        auto v=points[i-1];
        while(hull.size()>lower&&Cross(hull[hull.size()-2],hull.back(),v)<=0) {
            hull.pop_back();
        }
        hull.push_back(v);
    }
    if (hull.size()>1) {
        hull.pop_back();
    }
    float distance=1.0e30f;
    bool inside=hull.size()>2;
    for (std::size_t i=0;i<hull.size();++i) {
        const auto a=hull[i],b=hull[(i+1)%hull.size()];
        const float dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
        const float t=length>0 ? std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/length,0.0f,1.0f) : 0;
        const float ex=p.x-a.x-t*dx,ey=p.y-a.y-t*dy;
        distance=std::min(distance,std::sqrt(ex*ex+ey*ey));
        if(Cross(a,b,p)<0) {
            inside=false;
        }
    }
    return (inside ? distance : -distance)+padding;
}
}
struct RagdollControls::Impl {
    explicit Impl(RagdollRig& value):rig(value) {}
    RagdollRig& rig;
    struct Target {std::string bone; RagdollBodyTarget value;};
    struct Force {int body; math::Vector3 value,point; float remaining; bool torque,atPoint;};
    struct Grip {int body; physics::RigidBody* target; std::weak_ptr<const void> targetLifetime; std::unique_ptr<physics::XPBDGrip> value;};
    std::vector<Target> targets;
    std::vector<Force> forces;
    std::vector<Grip> grips;
    struct Exclusion {physics::RigidBody* body;std::weak_ptr<const void> lifetime;};
    std::vector<Exclusion> exclusions;
    int Find(std::string_view bone) const {
        for(std::size_t i=0;i<rig.m_bodies.size();++i) if(rig.m_bodies[i].name==bone) return static_cast<int>(i);
        return -1;
    }
};
RagdollControls::RagdollControls(RagdollRig& rig):m_impl(std::make_unique<Impl>(rig)) {}
RagdollControls::~RagdollControls()=default;
void RagdollControls::Reset() {m_impl->targets.clear();m_impl->forces.clear();m_impl->grips.clear();m_impl->exclusions.clear();}
std::string RagdollControls::BodyName(int index) const
{return index>=0&&index<static_cast<int>(m_impl->rig.m_bodies.size()) ? m_impl->rig.m_bodies[static_cast<std::size_t>(index)].name : std::string{};}
RagdollBodyContact RagdollControls::Contact(std::string_view bone,int index) const
{
    RagdollBodyContact out;
    const auto& rig=m_impl->rig;
    const int body=m_impl->Find(bone);
    if(body<0||index<0) {
        return out;
    }
    auto* requested=rig.m_bodies[static_cast<std::size_t>(body)].body.get();
    for(std::size_t i=0;i<rig.m_contacts.size()&&i<rig.m_contactCache.size();++i) {
        const auto& c=rig.m_contacts[i];
        const auto& cache=rig.m_contactCache[i];
        if (cache.bodyA.expired() || (c.GetOther() && cache.bodyB.expired())) {
            continue;
        }
        if (cache.lifetimeA.expired() || cache.lifetimeB.expired()) {
            continue;
        }
        const bool requestedIsBody = c.GetBody() == requested;
        const bool requestedIsOther = c.IsOtherSolved() && c.GetOther() == requested;
        if ((!requestedIsBody && !requestedIsOther) || !c.IsTouching()) {
            continue;
        }
        if(index-->0) {
            continue;
        }
        auto* other = requestedIsBody ? c.GetOther() : c.GetBody();
        const int own=rig.OwnBodyIndex(other);
        out.valid=true;out.self=own>=0;out.dynamic=other&&!other->IsStatic();
        out.point=c.ContactPointWorld();out.normal=requestedIsBody ? c.GetNormal() : -c.GetNormal();
        out.penetration=c.GetPenetration();
        if(other) {
            out.otherVelocity=other->GetVelocity()+math::Vector3::Cross(other->GetAngularVelocity(),out.point-other->GetPosition());
        }
        if(own>=0) {
            out.otherBone=rig.m_bodies[static_cast<std::size_t>(own)].name;
        }
        return out;
    }
    return out;
}
RagdollBodyState RagdollControls::BodyState(std::string_view bone) const
{
    RagdollBodyState out;
    const auto& rig=m_impl->rig;
    const int index=m_impl->Find(bone);
    if(index<0) {
        return out;
    }
    const auto& link=rig.m_bodies[static_cast<std::size_t>(index)];
    const auto& body=*link.body;
    out.valid=true;out.mass=body.GetMass();out.position=body.GetPosition();out.rotation=body.GetRotation();
    out.velocity=body.GetVelocity();out.angularVelocity=body.GetAngularVelocity();
    const auto up=-rig.m_solver.GetGravity().NormalizedOr(-math::Vector3::UP);
    for(int i=0;;++i) {auto c=Contact(bone,i);if(!c.valid) break; ++out.contacts;out.supported=out.supported||(!c.self&&math::Vector3::Dot(c.normal,up)>=0.5f);}
    out.supported=out.supported || (rig.m_groundHeight>-5.0e8f&&link.Bottom(out.position,out.rotation)<=rig.m_groundHeight+0.002f);
    return out;
}
bool RagdollControls::BodyDrive(std::string_view bone,RagdollBodyDrive& out) const
{const int i=m_impl->Find(bone);if(i<0)return false;out=m_impl->rig.m_bodies[static_cast<std::size_t>(i)].activeDrive;return true;}
RagdollDriveSpace RagdollControls::Space(std::string_view bone,RagdollDriveSpace fallback) const
{for(const auto& t:m_impl->targets)if(t.bone==bone)return t.value.space;return fallback;}
bool RagdollControls::SetTarget(std::string_view bone,const RagdollBodyTarget& target)
{
    const int bodyIndex = m_impl->Find(bone);
    if (bodyIndex < 0) {
        return false;
    }

    // 正規化やブレンドの前に、姿勢・速度・重みが有効かを確認する。
    if (!Finite(target.position) || !Finite(target.velocity) || !Finite(target.angularVelocity)) {
        return false;
    }
    if (!Finite(target.rotation)) {
        return false;
    }
    const float rotationLength = target.rotation.Length();
    if (!std::isfinite(rotationLength) || rotationLength < 1.0e-5f) {
        return false;
    }
    if (!std::isfinite(target.positionWeight) || target.positionWeight < 0.0f || target.positionWeight > 1.0f) {
        return false;
    }
    if (!std::isfinite(target.rotationWeight) || target.rotationWeight < 0.0f || target.rotationWeight > 1.0f) {
        return false;
    }
    if (target.space != RagdollDriveSpace::WORLD && target.space != RagdollDriveSpace::PARENT) {
        return false;
    }

    // 新規の目標や座標系を変更した目標には、それ以前の姿勢履歴を引き継がない。
    auto normalizedTarget = target;
    normalizedTarget.rotation = normalizedTarget.rotation.Normalized();
    auto& targets = m_impl->targets;
    const auto existing = std::find_if(targets.begin(), targets.end(), [bone](const auto& entry) {
        return entry.bone == bone;
    });
    const bool resetHistory = existing == targets.end() || existing->value.space != normalizedTarget.space;
    if (existing == targets.end()) {
        targets.push_back({std::string(bone), normalizedTarget});
    } else {
        existing->value = normalizedTarget;
    }
    auto& rig = m_impl->rig;
    if (resetHistory) {
        rig.m_bodies[static_cast<std::size_t>(bodyIndex)].targetHistory = false;
    }
    rig.ResetRest();
    rig.ApplyDrive();
    return true;
}
bool RagdollControls::ClearTarget(std::string_view bone)
{
    const int i=m_impl->Find(bone);if(i<0)return false;
    std::erase_if(m_impl->targets,[&](const auto& t){return t.bone==bone;});
    m_impl->rig.m_bodies[static_cast<std::size_t>(i)].targetHistory=false;m_impl->rig.ApplyDrive();return true;
}
void RagdollControls::ClearTargets()
{m_impl->targets.clear();for(auto& b:m_impl->rig.m_bodies)b.targetHistory=false;m_impl->rig.ApplyDrive();}
void RagdollControls::BlendTarget(std::string_view bone,math::Vector3& p,math::Quaternion& q,math::Vector3& v,math::Vector3& w) const
{
    for(const auto& t:m_impl->targets)if(t.bone==bone) {
        const auto& a=t.value;
        p=p+(a.position-p)*a.positionWeight;q=math::Quaternion::Slerp(q,a.rotation,a.rotationWeight);
        v=v+(a.velocity-v)*a.positionWeight;w=w+(a.angularVelocity-w)*a.rotationWeight;return;
    }
}
bool RagdollControls::Impulse(std::string_view bone,const math::Vector3& value,const math::Vector3* point)
{
    const int bodyIndex = m_impl->Find(bone);
    if (bodyIndex < 0 || !Finite(value) || (point && !Finite(*point))) {
        return false;
    }

    auto& body = *m_impl->rig.m_bodies[static_cast<std::size_t>(bodyIndex)].body;
    if (point) {
        body.ApplyImpulseAtPoint(value, *point);
    } else {
        body.ApplyImpulse(value);
    }
    m_impl->rig.ResetRest();
    return true;
}
bool RagdollControls::AngularImpulse(std::string_view bone,const math::Vector3& value)
{
    const int bodyIndex = m_impl->Find(bone);
    if (bodyIndex < 0 || !Finite(value)) {
        return false;
    }

    m_impl->rig.m_bodies[static_cast<std::size_t>(bodyIndex)].body->ApplyAngularImpulse(value);
    m_impl->rig.ResetRest();
    return true;
}
bool RagdollControls::Force(std::string_view bone,const math::Vector3& value,float seconds,const math::Vector3* point)
{
    const int bodyIndex = m_impl->Find(bone);
    if (bodyIndex < 0 || !Finite(value) || (point && !Finite(*point))) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds <= 0.0f) {
        return false;
    }
    if (m_impl->forces.size() >= MAX_PENDING_FORCES) {
        return false;
    }

    m_impl->forces.push_back({bodyIndex, value, point ? *point : math::Vector3::ZERO, seconds, false, point != nullptr});
    m_impl->rig.ResetRest();
    return true;
}
bool RagdollControls::Torque(std::string_view bone,const math::Vector3& value,float seconds)
{
    if (!Force(bone, value, seconds)) {
        return false;
    }
    m_impl->forces.back().torque = true;
    return true;
}
void RagdollControls::IntegrateForces(float h)
{
    if(!std::isfinite(h)||h<=0) {
        return;
    }
    if(!m_impl->forces.empty()) {
        m_impl->rig.ResetRest();
    }
    for (auto& force : m_impl->forces) {
        auto& body = *m_impl->rig.m_bodies[static_cast<std::size_t>(force.body)].body;
        // 力の持続時間で速度変化を求め、姿勢への反映にはソルバーの刻み幅を使う。
        const float appliedSeconds = std::min(h, force.remaining);
        const auto linearDelta = force.torque ? math::Vector3::ZERO : force.value * (body.GetInvMass() * appliedSeconds);
        auto torque = math::Vector3::ZERO;
        if (force.torque) {
            torque = force.value;
        } else if (force.atPoint) {
            torque = math::Vector3::Cross(force.point - body.GetPosition(), force.value);
        }
        const auto angularDelta = body.ApplyInvInertia(torque) * appliedSeconds;

        body.SetVelocity(body.GetVelocity() + linearDelta);
        body.SetPosition(body.GetPosition() + linearDelta * h);
        body.SetAngularVelocity(body.GetAngularVelocity() + angularDelta);
        body.SetRotation(physics::IntegrateRotation(body.GetRotation(), angularDelta, h));
        force.remaining -= appliedSeconds;
    }
    std::erase_if(m_impl->forces,[](const auto& f){return f.remaining<=0;});
}
bool RagdollControls::Grab(std::string_view bone, physics::RigidBody* other, const math::Vector3& point,
                           const physics::XPBDGripSettings& settings)
{
    const int bodyIndex = m_impl->Find(bone);
    if (bodyIndex < 0 || m_impl->rig.OwnBodyIndex(other) >= 0) {
        return false;
    }

    // 新しい拘束の準備に失敗しても、この部位が既に保持している拘束を保つ。
    auto newGrip = std::make_unique<physics::XPBDGrip>();
    auto* grippingBody = m_impl->rig.m_bodies[static_cast<std::size_t>(bodyIndex)].body.get();
    if (!newGrip->Bind(grippingBody, other, point, settings)) {
        return false;
    }

    // 剛体を掴む場合は失効した接触除外を取り除き、この対象の除外を一度だけ登録する。
    if (other) {
        auto& exclusions = m_impl->exclusions;
        std::erase_if(exclusions, [](const auto& exclusion) {
            return exclusion.lifetime.expired();
        });
        const bool alreadyExcluded = std::any_of(exclusions.begin(), exclusions.end(), [&](const auto& exclusion) {
            return exclusion.body == other;
        });
        if (!alreadyExcluded) {
            exclusions.push_back({other, other->ObserveLifetime()});
        }
    }

    // 一時登録中の拘束への参照を保つため、同じ部位の拘束は実体を残したまま更新する。
    for (auto& existingGrip : m_impl->grips) {
        if (existingGrip.body != bodyIndex) {
            continue;
        }

        *existingGrip.value = std::move(*newGrip);
        existingGrip.target = other;
        existingGrip.targetLifetime = other ? other->ObserveLifetime() : std::weak_ptr<const void>{};
        m_impl->rig.ResetRest();
        return true;
    }

    m_impl->grips.push_back({bodyIndex, other,
        other ? other->ObserveLifetime() : std::weak_ptr<const void>{}, std::move(newGrip)});
    m_impl->rig.ResetRest();
    return true;
}
bool RagdollControls::Release(std::string_view bone)
{const int i=m_impl->Find(bone);if(i<0)return false;for(auto& g:m_impl->grips)if(g.body==i)g.value->Release();return true;}
void RagdollControls::ReleaseAll() {for(auto& g:m_impl->grips)g.value->Release();}
RagdollGrabState RagdollControls::GrabState(std::string_view bone) const
{const int i=m_impl->Find(bone);for(const auto& g:m_impl->grips)if(g.body==i)return {g.value->Status(),g.value->Anchor(),g.value->Target(),g.value->Force()};return {};}
bool RagdollControls::HasGrabs() const
{for(const auto& g:m_impl->grips)if(g.value->Status()==physics::XPBDGripStatus::HOLDING)return true;return false;}
bool RagdollControls::IgnoresContactsWith(const physics::RigidBody* body) const
{
    if(!body) {
        return false;
    }
    for(const auto& x:m_impl->exclusions)
        if(x.body==body&&!x.lifetime.expired()&&!body->IsStatic()) {
            return true;
        }
    return false;
}
void RagdollControls::UpdateContactExclusions(physics::World& world)
{
    const auto isHeld = [&](const physics::RigidBody* body) {
        return std::any_of(m_impl->grips.begin(), m_impl->grips.end(), [&](const auto& grip) {
            return grip.target == body && !grip.targetLifetime.expired()
                && grip.value->Status() == physics::XPBDGripStatus::HOLDING;
        });
    };

    // 手放した動的剛体が残っている場合だけ、再接触を確認するため周囲の候補を集める。
    const auto& rigInstances = m_impl->rig.m_instances;
    const bool needsSeparationCheck = std::any_of(m_impl->exclusions.begin(), m_impl->exclusions.end(), [&](const auto& exclusion) {
        return !exclusion.lifetime.expired() && !isHeld(exclusion.body) && !exclusion.body->IsStatic();
    });
    constexpr float RELEASE_CONTACT_PADDING = 0.02f;
    const float clearance = RELEASE_CONTACT_PADDING + std::max(0.0f, m_impl->rig.m_contactSettings.margin);
    std::vector<const physics::ColliderInstance*> candidates;
    if (needsSeparationCheck && !rigInstances.empty()) {
        auto bounds = rigInstances.front().collider->GetAABB();
        for (const auto& instance : rigInstances) {
            bounds = bounds.Merge(instance.collider->GetAABB());
        }
        candidates = world.OverlapSphere(bounds.Center(), bounds.Extents().Length() + clearance);
    }

    // 重なりがなくなるまで除外を保ち、手放した直後の押し戻しを防ぐ。
    std::erase_if(m_impl->exclusions, [&](const auto& exclusion) {
        if (exclusion.lifetime.expired()) {
            return true;
        }
        if (isHeld(exclusion.body)) {
            return false;
        }
        if (exclusion.body->IsStatic()) {
            return true;
        }
        for (const auto* candidate : candidates) {
            if (candidate->body != exclusion.body || !candidate->collider || candidate->isTrigger) {
                continue;
            }
            const auto candidateBounds = candidate->collider->GetAABB();
            for (const auto& instance : rigInstances) {
                auto paddedRigBounds = instance.collider->GetAABB();
                const math::Vector3 padding{clearance, clearance, clearance};
                paddedRigBounds.min -= padding;
                paddedRigBounds.max += padding;
                if (paddedRigBounds.Overlaps(candidateBounds)) {
                    return false;
                }
            }
        }
        return true;
    });
}
void RagdollControls::RegisterGrabs(physics::XPBDSolver& solver)
{
    // 接触再構築で消えた一時制約に、現在保持している掴みだけを登録し直す。
    for (auto& grip : m_impl->grips) {
        if (grip.value->Status() != physics::XPBDGripStatus::HOLDING) {
            continue;
        }
        solver.AddTransient(grip.value.get());
    }
}

RagdollBalanceState RagdollControls::Balance(float padding,float minSupportDot) const
{
    RagdollBalanceState out;
    if (!std::isfinite(padding) || padding < 0.0f) {
        return out;
    }
    if (!std::isfinite(minSupportDot) || minSupportDot < 0.0f || minSupportDot > 1.0f) {
        return out;
    }
    const auto& rig=m_impl->rig;
    out.gravity=rig.m_solver.GetGravity().Length();
    const auto up=-rig.m_solver.GetGravity().NormalizedOr(-math::Vector3::UP);
    auto tangent=math::Vector3::Cross(up,std::abs(up.x)<0.9f ? math::Vector3::RIGHT : math::Vector3::UP).Normalized();
    const auto bitangent=math::Vector3::Cross(up,tangent);
    std::vector<math::Vector3> support;
    for(const auto& link:rig.m_bodies) {
        const auto& b=*link.body;
        const float mass=std::max(0.0f,b.GetMass());
        out.mass+=mass;out.center+=b.GetPosition()*mass;out.velocity+=b.GetVelocity()*mass;
        for(int i=0;;++i) {
            auto c=Contact(link.name,i);if(!c.valid)break;
            if(c.self||math::Vector3::Dot(c.normal,up)<minSupportDot) {
                continue;
            }
            support.push_back(c.point);out.supportPoint+=c.point;out.supportNormal+=c.normal;out.supportVelocity+=c.otherVelocity;
        }
        if(rig.m_groundHeight>-5.0e8f&&link.Bottom(b.GetPosition(),b.GetRotation())<=rig.m_groundHeight+0.002f
            &&math::Vector3::Dot(math::Vector3::UP,up)>=minSupportDot) {
            const math::Vector3 p{b.GetPosition().x,rig.m_groundHeight,b.GetPosition().z};
            support.push_back(p);out.supportPoint+=p;out.supportNormal+=math::Vector3::UP;
        }
    }
    if(out.mass<=0) {
        return out;
    }
    out.valid=true;out.center=out.center/out.mass;out.velocity=out.velocity/out.mass;
    out.contacts=static_cast<int>(support.size());if(support.empty())return out;
    out.hasSupport=true;
    out.supportPoint=out.supportPoint/static_cast<float>(support.size());
    out.supportVelocity=out.supportVelocity/static_cast<float>(support.size());
    out.supportNormal=out.supportNormal.NormalizedOr(up);
    out.height=math::Vector3::Dot(out.center-out.supportPoint,up);
    const auto project=[&](const math::Vector3& v){return Point{math::Vector3::Dot(v-out.supportPoint,tangent),math::Vector3::Dot(v-out.supportPoint,bitangent)};};
    std::vector<Point> points;for(auto v:support)points.push_back(project(v));
    out.centerMargin=Margin(points,project(out.center),padding);
    if(out.gravity>1.0e-5f&&out.height>0.05f) {
        const auto velocity=out.velocity-out.supportVelocity;
        /// @see https://www.cs.cmu.edu/~cga/legs/Pratt_Goswami_Humanoids2006.pdf Capture Point, §V-C・式(10) の一定重心高さ近似。
        out.capturePoint=out.center-up*out.height+(velocity-up*math::Vector3::Dot(velocity,up))*std::sqrt(out.height/out.gravity);
        out.captureValid=true;out.captureMargin=Margin(points,project(out.capturePoint),padding);
    }
    return out;
}
}
