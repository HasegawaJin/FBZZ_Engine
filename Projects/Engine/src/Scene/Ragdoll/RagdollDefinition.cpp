/// @file    RagdollDefinition.cpp
/// @brief   モデル別物理設定の検証と TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Engine/Scene/Ragdoll/RagdollDefinition.hpp>
#include <Engine/Asset/AssetPath.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <toml++/toml.hpp>
#include <cmath>
#include <sstream>
#include <unordered_set>

namespace fbzz::scene {
namespace {
bool Fail(std::string* error, const std::string& message)
{
    if (error) *error = message;
    return false;
}
bool Finite(const math::Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool Rotation(const math::Quaternion& q)
{
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w)
        && q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w > 1.0e-8f;
}
toml::array Array(const math::Vector3& v) { return toml::array{v.x, v.y, v.z}; }
toml::array Array(const math::Quaternion& q) { return toml::array{q.x, q.y, q.z, q.w}; }
bool ReadVector(const toml::table& t, std::string_view key, math::Vector3& v)
{
    const auto node = t[key];
    if (!node) return true;
    const auto* a = node.as_array();
    if (!a || a->size() != 3) return false;
    float* fields[] = {&v.x, &v.y, &v.z};
    for (std::size_t i=0; i<3; ++i) {
        const auto f = (*a)[i].value<double>();
        if (!f) return false;
        *fields[i] = static_cast<float>(*f);
    }
    return true;
}
bool ReadRotation(const toml::table& t, std::string_view key, math::Quaternion& q)
{
    if (!t[key]) return true;
    const auto* a = t[key].as_array();
    if (!a || a->size()!=4) return false;
    float* fields[] = {&q.x,&q.y,&q.z,&q.w};
    for (std::size_t i=0;i<4;++i) {
        const auto f=(*a)[i].value<double>();
        if (!f) return false;
        *fields[i]=static_cast<float>(*f);
    }
    return true;
}
}

bool ValidateRagdollDefinition(const RagdollDefinition& value, std::string* error)
{
    std::unordered_set<std::string> names;
    if (value.bodies.empty()) return Fail(error,"No rigid bodies");
    for (const auto& b : value.bodies) {
        if (b.bone.empty() || !names.insert(b.bone).second) return Fail(error,"Empty or duplicate bone: "+b.bone);
        if (!Finite(b.center) || !Finite(b.jointAnchor) || !Rotation(b.rotation) || !Rotation(b.jointRotation))
            return Fail(error,"Invalid body/joint transform: "+b.bone);
        if (!std::isfinite(b.mass) || b.mass<0 || !Finite(b.inertia)
            || !std::isfinite(b.settings.density) || b.settings.density<=0)
            return Fail(error,"Invalid mass/density: "+b.bone);
        if (b.inertia.LengthSq()!=0 && (b.inertia.x<=0 || b.inertia.y<=0 || b.inertia.z<=0))
            return Fail(error,"Inertia must be all zero or all positive: "+b.bone);
        if ((b.shape==RagdollBodyShape::CAPSULE || b.shape==RagdollBodyShape::SPHERE)
            && (!std::isfinite(b.radius) || b.radius<=0)) return Fail(error,"Invalid radius: "+b.bone);
        if (b.shape==RagdollBodyShape::CAPSULE && (!std::isfinite(b.halfHeight) || b.halfHeight<0))
            return Fail(error,"Invalid half height: "+b.bone);
        if (b.shape==RagdollBodyShape::BOX && (!Finite(b.halfExtents)
            || b.halfExtents.x<=0 || b.halfExtents.y<=0 || b.halfExtents.z<=0)) return Fail(error,"Invalid box: "+b.bone);
        if (b.shape!=RagdollBodyShape::CAPSULE && b.shape!=RagdollBodyShape::SPHERE && b.shape!=RagdollBodyShape::BOX)
            return Fail(error,"Unknown shape: "+b.bone);
        const auto& d=b.drive;
        const float fields[]={d.linearFrequency,d.angularFrequency,d.linearDampingRatio,d.angularDampingRatio,
            d.maxForce,d.maxTorque,d.targetVelocityMultiplier,b.settings.servo.torqueScale,b.settings.servo.holdSag,b.settings.servo.damping};
        for (float f:fields) if (!std::isfinite(f)||f<0) return Fail(error,"Invalid drive: "+b.bone);
        if (d.space!=RagdollDriveSpace::PARENT && d.space!=RagdollDriveSpace::WORLD) return Fail(error,"Unknown drive space");
        const auto& l=b.settings.limits;
        if (!std::isfinite(l.compliance) || l.compliance<0) return Fail(error,"Invalid limit compliance");
        const float angles[]={l.twistMin,l.twistMax,l.swingMinY,l.swingMaxY,l.swingMinZ,l.swingMaxZ};
        for (float f:angles) if (!std::isfinite(f)) return Fail(error,"Invalid joint limit: "+b.bone);
        if (l.twistMin>l.twistMax || l.swingMinY>l.swingMaxY || l.swingMinZ>l.swingMaxZ)
            return Fail(error,"Reversed joint limit: "+b.bone);
    }
    for (const auto& p:value.collisionExclusions)
        if (p.bodyA==p.bodyB || !names.contains(p.bodyA) || !names.contains(p.bodyB))
            return Fail(error,"Unknown/self collision exclusion");
    if (error) error->clear();
    return true;
}

bool LoadRagdollDefinition(const std::string& reference, RagdollDefinition& out, std::string* error)
{
    std::string text;
    if (!util::FileSystem::ReadText(asset::ResolveAssetPath(reference),text)) return Fail(error,"Cannot read "+reference);
    const auto parsed=toml::parse(text);
    if (!parsed) return Fail(error,"Invalid TOML: "+reference);
    const auto& root=parsed.table();
    const auto* header=root["ragdoll"].as_table();
    if (!header || (*header)["version"].value_or(0)!=1) return Fail(error,"Unsupported ragdoll version");
    RagdollDefinition value;
    value.name=(*header)["name"].value_or(std::string{});
    value.skeletonSource=(*header)["skeleton_source"].value_or(std::string{});
    value.definitionsOnly=(*header)["definitions_only"].value_or(true);
    const auto* bodies=root["bodies"].as_array();
    if (!bodies) return Fail(error,"Missing bodies");
    for (const auto& node:*bodies) {
        const auto* t=node.as_table();
        if (!t) return Fail(error,"Invalid body table");
        RagdollBodyDefinition b;
        b.bone=(*t)["bone"].value_or(std::string{});
        for (const char* key:{"radius","half_height","mass","density","twist_min","twist_max","swing_y_min","swing_y_max",
            "swing_z_min","swing_z_max","limit_compliance","servo_torque_scale","servo_hold_sag","servo_damping",
            "linear_frequency","angular_frequency","linear_damping_ratio","angular_damping_ratio","max_force","max_torque","target_velocity_multiplier"})
            if ((*t)[key] && !(*t)[key].is_number()) return Fail(error,std::string("Expected number: ")+key);
        for (const char* key:{"limits_enabled","servo_enabled","drive_enabled"})
            if ((*t)[key] && !(*t)[key].is_boolean()) return Fail(error,std::string("Expected boolean: ")+key);
        const auto shape=(*t)["shape"].value_or(std::string{"capsule"});
        if (shape=="sphere") b.shape=RagdollBodyShape::SPHERE;
        else if (shape=="box") b.shape=RagdollBodyShape::BOX;
        else if (shape!="capsule") return Fail(error,"Unknown shape: "+shape);
        if (!ReadVector(*t,"center",b.center) || !ReadVector(*t,"half_extents",b.halfExtents)
            || !ReadVector(*t,"inertia",b.inertia) || !ReadVector(*t,"joint_anchor",b.jointAnchor)
            || !ReadRotation(*t,"rotation",b.rotation) || !ReadRotation(*t,"joint_rotation",b.jointRotation))
            return Fail(error,"Invalid vector/quaternion: "+b.bone);
        b.radius=(*t)["radius"].value_or(b.radius);
        b.halfHeight=(*t)["half_height"].value_or(b.halfHeight);
        b.mass=(*t)["mass"].value_or(b.mass);
        b.settings.density=(*t)["density"].value_or(b.settings.density);
        auto& l=b.settings.limits;
        l.enabled=(*t)["limits_enabled"].value_or(true);
        l.compliance=(*t)["limit_compliance"].value_or(0.0f);
        l.twistMin=(*t)["twist_min"].value_or(l.twistMin*math::RAD2DEG)*math::DEG2RAD;
        l.twistMax=(*t)["twist_max"].value_or(l.twistMax*math::RAD2DEG)*math::DEG2RAD;
        l.swingMinY=(*t)["swing_y_min"].value_or(l.swingMinY*math::RAD2DEG)*math::DEG2RAD;
        l.swingMaxY=(*t)["swing_y_max"].value_or(l.swingMaxY*math::RAD2DEG)*math::DEG2RAD;
        l.swingMinZ=(*t)["swing_z_min"].value_or(l.swingMinZ*math::RAD2DEG)*math::DEG2RAD;
        l.swingMaxZ=(*t)["swing_z_max"].value_or(l.swingMaxZ*math::RAD2DEG)*math::DEG2RAD;
        b.settings.servo.enabled=(*t)["servo_enabled"].value_or(false);
        b.settings.servo.torqueScale=(*t)["servo_torque_scale"].value_or(b.settings.servo.torqueScale);
        b.settings.servo.holdSag=(*t)["servo_hold_sag"].value_or(b.settings.servo.holdSag);
        b.settings.servo.damping=(*t)["servo_damping"].value_or(b.settings.servo.damping);
        auto& d=b.drive;
        d.enabled=(*t)["drive_enabled"].value_or(false);
        const auto space=(*t)["drive_space"].value_or(std::string{"parent"});
        if (space=="world") d.space=RagdollDriveSpace::WORLD;
        else if (space!="parent") return Fail(error,"Unknown drive space");
        d.linearFrequency=(*t)["linear_frequency"].value_or(d.linearFrequency);
        d.angularFrequency=(*t)["angular_frequency"].value_or(d.angularFrequency);
        d.linearDampingRatio=(*t)["linear_damping_ratio"].value_or(d.linearDampingRatio);
        d.angularDampingRatio=(*t)["angular_damping_ratio"].value_or(d.angularDampingRatio);
        d.maxForce=(*t)["max_force"].value_or(d.maxForce);
        d.maxTorque=(*t)["max_torque"].value_or(d.maxTorque);
        d.targetVelocityMultiplier=(*t)["target_velocity_multiplier"].value_or(d.targetVelocityMultiplier);
        value.bodies.push_back(std::move(b));
    }
    if (const auto* exclusions=root["collision_exclusions"].as_array()) for (const auto& node:*exclusions) {
        const auto* t=node.as_table();
        if (!t) return Fail(error,"Invalid collision exclusion");
        value.collisionExclusions.push_back({(*t)["body_a"].value_or(std::string{}),(*t)["body_b"].value_or(std::string{})});
    }
    if (!ValidateRagdollDefinition(value,error)) return false;
    for (auto& b:value.bodies) { b.rotation=b.rotation.Normalized(); b.jointRotation=b.jointRotation.Normalized(); }
    value.signature=14695981039346656037ull;
    for (unsigned char c:text) { value.signature^=c; value.signature*=1099511628211ull; }
    out=std::move(value);
    return true;
}

bool SaveRagdollDefinition(const std::string& path, const RagdollDefinition& value, std::string* error)
{
    if (!ValidateRagdollDefinition(value,error)) return false;
    toml::table root;
    root.insert("ragdoll",toml::table{{"version",1},{"name",value.name},{"skeleton_source",value.skeletonSource},{"definitions_only",value.definitionsOnly}});
    toml::array bodies;
    for (const auto& b:value.bodies) {
        toml::table t{{"bone",b.bone},{"shape",b.shape==RagdollBodyShape::CAPSULE?"capsule":b.shape==RagdollBodyShape::SPHERE?"sphere":"box"},
            {"center",Array(b.center)},{"rotation",Array(b.rotation.Normalized())},{"radius",b.radius},{"half_height",b.halfHeight},
            {"half_extents",Array(b.halfExtents)},{"mass",b.mass},{"inertia",Array(b.inertia)},{"density",b.settings.density},
            {"joint_anchor",Array(b.jointAnchor)},{"joint_rotation",Array(b.jointRotation.Normalized())}};
        const auto& l=b.settings.limits;
        t.insert("limits_enabled",l.enabled); t.insert("limit_compliance",l.compliance);
        t.insert("twist_min",l.twistMin*math::RAD2DEG); t.insert("twist_max",l.twistMax*math::RAD2DEG);
        t.insert("swing_y_min",l.swingMinY*math::RAD2DEG); t.insert("swing_y_max",l.swingMaxY*math::RAD2DEG);
        t.insert("swing_z_min",l.swingMinZ*math::RAD2DEG); t.insert("swing_z_max",l.swingMaxZ*math::RAD2DEG);
        t.insert("servo_enabled",b.settings.servo.enabled); t.insert("servo_torque_scale",b.settings.servo.torqueScale);
        t.insert("servo_hold_sag",b.settings.servo.holdSag); t.insert("servo_damping",b.settings.servo.damping);
        const auto& d=b.drive;
        t.insert("drive_enabled",d.enabled); t.insert("drive_space",d.space==RagdollDriveSpace::WORLD?"world":"parent");
        t.insert("linear_frequency",d.linearFrequency); t.insert("angular_frequency",d.angularFrequency);
        t.insert("linear_damping_ratio",d.linearDampingRatio); t.insert("angular_damping_ratio",d.angularDampingRatio);
        t.insert("max_force",d.maxForce); t.insert("max_torque",d.maxTorque); t.insert("target_velocity_multiplier",d.targetVelocityMultiplier);
        bodies.push_back(std::move(t));
    }
    root.insert("bodies",std::move(bodies));
    toml::array exclusions;
    for (const auto& p:value.collisionExclusions) exclusions.push_back(toml::table{{"body_a",p.bodyA},{"body_b",p.bodyB}});
    root.insert("collision_exclusions",std::move(exclusions));
    std::ostringstream stream; stream<<root;
    return util::FileSystem::WriteTextAtomic(path,stream.str()) || Fail(error,"Cannot save "+path);
}

void ApplyRagdollDefinition(const RagdollDefinition& value, RagdollProfile& profile)
{
    profile.definitionsOnly=value.definitionsOnly;
    profile.bodies=value.bodies;
    profile.collisionExclusions=value.collisionExclusions;
    if (value.definitionsOnly) { profile.excludeNames.clear(); profile.excludePatterns.clear(); profile.noBodyPatterns.clear(); }
}
}
