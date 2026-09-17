/// @file    CameraRigComponents.hpp
/// @brief   Virtual Cameraの優先度選択、追従、ブレンド、揺れを構成するComponent。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Math/Quaternion.hpp>

namespace fbzz::scene {

struct VirtualCameraComponent {
    bool enabled = true;
    int priority = 0;
    float fovY = 60.0f;
    float blendInTime = 0.35f;
    bool active = false;

    const char* GetTypeName() const { return "Virtual Camera"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("priority", priority);
        r.FloatRange("fovY", fovY, 1.0f, 179.0f);
        r.Field("blendInTime", blendInTime);
        r.Readonly("active", active);
    }
};

struct CameraFollowComponent {
    bool enabled = true;
    EntityRef target;
    math::Vector3 offset = { 0.0f, 2.0f, -5.0f };
    float positionDamping = 0.15f;
    float rotationDamping = 0.1f;
    bool lookAtTarget = true;
    bool useTargetRotation = false;

    const char* GetTypeName() const { return "Camera Follow"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("target", target);
        r.Field("offset", offset);
        r.Field("positionDamping", positionDamping);
        r.Field("rotationDamping", rotationDamping);
        r.Field("lookAtTarget", lookAtTarget);
        r.Field("useTargetRotation", useTargetRotation);
    }
};

enum class CameraBlendCurve : int { Linear = 0, EaseInOut = 1, Cut = 2 };

struct CameraBlendComponent {
    bool enabled = true;
    EntityRef fromCamera;
    EntityRef toCamera;
    float duration = 0.35f;
    float elapsed = 0.0f;
    CameraBlendCurve curve = CameraBlendCurve::EaseInOut;
    bool playing = false;

    const char* GetTypeName() const { return "Camera Blend"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("fromCamera", fromCamera);
        r.Field("toCamera", toCamera);
        r.Field("duration", duration);
        int value = static_cast<int>(curve);
        static constexpr const char* CURVES[] = { "Linear", "Ease In Out", "Cut" };
        r.Enum("curve", value, CURVES);
        curve = static_cast<CameraBlendCurve>(value < 0 || value > 2 ? 1 : value);
        r.Field("playing", playing);
        r.Readonly("elapsed", elapsed);
    }
};

struct CameraShakeComponent {
    bool enabled = true;
    float amplitude = 0.15f;
    float rotationAmplitudeDegrees = 1.0f;
    float frequency = 20.0f;
    float duration = 0.3f;
    float elapsed = 0.0f;
    float falloffPower = 2.0f;
    int seed = 1;
    bool playOnAwake = false;
    bool playing = false;
    /// ランタイムで前フレームの揺れを除去してから次の揺れを加えるための非永続状態。
    math::Vector3 appliedPositionOffset = math::Vector3::ZERO;
    math::Quaternion appliedRotationOffset = math::Quaternion::Identity();
    bool runtimeInitialized = false;

    const char* GetTypeName() const { return "Camera Shake"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("amplitude", amplitude);
        r.Field("rotationAmplitudeDegrees", rotationAmplitudeDegrees);
        r.Field("frequency", frequency);
        r.Field("duration", duration);
        r.Field("falloffPower", falloffPower);
        r.Field("seed", seed);
        r.Field("playOnAwake", playOnAwake);
        r.Field("playing", playing);
        r.Readonly("elapsed", elapsed);
    }
};

} // namespace fbzz::scene
