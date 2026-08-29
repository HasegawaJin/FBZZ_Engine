/// @file    RigidBodySerializer.cpp
/// @brief   RigidBody の TOML シリアライズ / デシリアライズ。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#include <Physics/RigidBodySerializer.hpp>
#include <toml++/toml.hpp>
#include <sstream>

namespace fbzz::physics
{

namespace
{

toml::array Vec3ToArr(const math::Vector3& v)
{
    toml::array a;
    a.push_back((double)v.x);
    a.push_back((double)v.y);
    a.push_back((double)v.z);
    return a;
}

toml::array QuatToArr(const math::Quaternion& q)
{
    toml::array a;
    a.push_back((double)q.x);
    a.push_back((double)q.y);
    a.push_back((double)q.z);
    a.push_back((double)q.w);
    return a;
}

math::Vector3 ArrToVec3(const toml::array* arr, math::Vector3 def = {})
{
    if (!arr || arr->size() < 3) return def;
    return {
        (float)(*arr)[0].value_or(0.0),
        (float)(*arr)[1].value_or(0.0),
        (float)(*arr)[2].value_or(0.0)
    };
}

math::Quaternion ArrToQuat(const toml::array* arr)
{
    if (!arr || arr->size() < 4) return { 0.0f, 0.0f, 0.0f, 1.0f };
    return {
        (float)(*arr)[0].value_or(0.0),
        (float)(*arr)[1].value_or(0.0),
        (float)(*arr)[2].value_or(0.0),
        (float)(*arr)[3].value_or(1.0)
    };
}

} // namespace

// ---------------------------------------------------------------------------
// Serialize
// ---------------------------------------------------------------------------
std::string RigidBodySerializer::Serialize(const RigidBody& body)
{
    toml::table tbl;

    // 状態
    tbl.insert("position",         Vec3ToArr(body.GetPosition()));
    tbl.insert("velocity",         Vec3ToArr(body.GetVelocity()));
    tbl.insert("rotation",         QuatToArr(body.GetRotation()));
    tbl.insert("angular_velocity", Vec3ToArr(body.GetAngularVelocity()));
    tbl.insert("freeze_position",  Vec3ToArr({
        body.GetFreezePosition().x ? 1.0f : 0.0f,
        body.GetFreezePosition().y ? 1.0f : 0.0f,
        body.GetFreezePosition().z ? 1.0f : 0.0f
    }));
    tbl.insert("freeze_rotation",  Vec3ToArr({
        body.GetFreezeRotation().x ? 1.0f : 0.0f,
        body.GetFreezeRotation().y ? 1.0f : 0.0f,
        body.GetFreezeRotation().z ? 1.0f : 0.0f
    }));

    // 基本設定
    tbl.insert("mass",      (double)body.GetMass());
    tbl.insert("is_static", body.IsStatic());

    // 拡張プロパティ (デフォルト値なら省略)
    if (!body.m_useGravity)
        tbl.insert("use_gravity", false);
    if (body.m_gravityScale != 1.0f)
        tbl.insert("gravity_scale", (double)body.m_gravityScale);
    if (body.m_linearDrag != 0.0f)
        tbl.insert("linear_drag", (double)body.m_linearDrag);
    if (body.m_angularDrag != 0.0f)
        tbl.insert("angular_drag", (double)body.m_angularDrag);
    if (!body.m_allowSleeping)
        tbl.insert("allow_sleeping", false);
    if (body.m_useCCD)
    {
        tbl.insert("use_ccd", true);
        tbl.insert("ccd_radius", (double)body.m_ccdRadius);
    }
    if (body.m_charge != 0.0f)
        tbl.insert("charge", (double)body.m_charge);
    if (body.m_isGravitationalSource)
    {
        tbl.insert("is_gravitational_source", true);
        tbl.insert("gravitational_mass", (double)body.m_gravitationalMass);
    }

    std::ostringstream oss;
    oss << tbl;
    return oss.str();
}

// ---------------------------------------------------------------------------
// Deserialize
// ---------------------------------------------------------------------------
bool RigidBodySerializer::Deserialize(RigidBody& body, std::string_view tomlText)
{
    auto result = toml::parse(tomlText);
    if (!result) return false;
    const toml::table& tbl = result.table();

    // is_static を先に設定 → SetMass 内の invMass 計算に影響する
    body.m_isStatic = tbl["is_static"].value_or(false);
    body.SetMass((float)tbl["mass"].value_or(1.0));

    // 状態
    body.SetPosition(ArrToVec3(tbl["position"].as_array()));
    body.SetVelocity(ArrToVec3(tbl["velocity"].as_array()));
    body.SetRotation(ArrToQuat(tbl["rotation"].as_array()));
    body.SetAngularVelocity(ArrToVec3(tbl["angular_velocity"].as_array()));
    const math::Vector3 freezePosition = ArrToVec3(tbl["freeze_position"].as_array(), math::Vector3::ZERO);
    const math::Vector3 freezeRotation = ArrToVec3(tbl["freeze_rotation"].as_array(), math::Vector3::ZERO);
    body.SetFreezePosition({
        freezePosition.x != 0.0f,
        freezePosition.y != 0.0f,
        freezePosition.z != 0.0f
    });
    body.SetFreezeRotation({
        freezeRotation.x != 0.0f,
        freezeRotation.y != 0.0f,
        freezeRotation.z != 0.0f
    });

    // 拡張プロパティ
    body.m_useGravity            = tbl["use_gravity"].value_or(true);
    body.m_gravityScale          = (float)tbl["gravity_scale"].value_or(1.0);
    body.m_linearDrag            = (float)tbl["linear_drag"].value_or(0.0);
    body.m_angularDrag           = (float)tbl["angular_drag"].value_or(0.0);
    body.m_allowSleeping         = tbl["allow_sleeping"].value_or(true);
    body.m_useCCD                = tbl["use_ccd"].value_or(false);
    body.m_ccdRadius             = (float)tbl["ccd_radius"].value_or(0.5);
    body.m_charge                = (float)tbl["charge"].value_or(0.0);
    body.m_isGravitationalSource = tbl["is_gravitational_source"].value_or(false);
    body.m_gravitationalMass     = (float)tbl["gravitational_mass"].value_or(1.0);

    return true;
}

} // namespace fbzz::physics
