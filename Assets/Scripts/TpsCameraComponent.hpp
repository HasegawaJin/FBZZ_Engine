// FBZZ Engine
// TpsCameraComponent.hpp | sandbox
// プレイヤーを追従する三人称カメラスクリプト
#pragma once

// WHY: Sandbox スクリプトは engine 層からインクルードされない末端ヘッダのため、
//      using namespace を許可する。詳細は AGENTS.md を参照。
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;

namespace sandbox {

// プレイヤーなどのターゲットを一定距離から追従する三人称カメラを制御する。
class TpsCameraComponent : public Script {
public:
    static constexpr const char* TYPE_NAME = "TpsCameraComponent";

    const char* GetTypeName() const override { return TYPE_NAME; }

    std::string targetTag = "Player";
    float distance = 5.0f;
    float height = 1.6f;
    float yaw = 0.0f;
    float pitch = 15.0f;
    float minPitch = -20.0f;
    float maxPitch = 65.0f;
    float mouseSensitivity = 0.2f;
    bool mouseOrbit = true;
    float followSpeed = 10.0f;

    // Inspector / シーン保存用に、TPS カメラの調整パラメータを公開する。
    void Reflect(IReflector& reflector) override
    {
        reflector.Field("Target Tag",        targetTag);
        reflector.Field("Distance",          distance);
        reflector.Field("Height",            height);
        reflector.Field("Yaw",               yaw);
        reflector.Field("Pitch",             pitch);
        reflector.Field("Min Pitch",         minPitch);
        reflector.Field("Max Pitch",         maxPitch);
        reflector.Field("Mouse Sensitivity", mouseSensitivity);
        reflector.Field("Mouse Orbit",       mouseOrbit);
        reflector.Field("Follow Speed",      followSpeed);
    }

    // 実行開始時にターゲットを解決し、初回 LateUpdate で正しい位置へスナップできる状態にする。
    void OnStart() override
    {
        FindTarget();
        m_hasCameraPosition = false;
    }

    // WHY: PhysicsSystem 後の最新プレイヤー位置を使うことで、カメラ位置と
    //      プレイヤーメッシュ位置の 1 フレームずれによる前後ジッターを防ぐ。
    void OnLateUpdate(float dt) override
    {
        if (!transform) return;
        if (!m_target || !m_target->IsValid()) FindTarget();
        if (!m_target) return;

        if (mouseOrbit && input.MouseButton(1)) {
            const Vector2 delta = input.GetMouseDelta();
            yaw   += delta.x * mouseSensitivity;
            pitch  = Clamp(pitch + delta.y * mouseSensitivity, minPitch, maxPitch);
        }

        const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(yaw));
        const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT, ToRad(pitch));
        const Quaternion rotation = (yawRot * pitchRot).Normalized();
        const Vector3    focus    = m_target->transform.position + Vector3::UP * height;
        const Vector3    targetCamPos = focus - (rotation * Vector3::FORWARD) * distance;

        // WHAT: followSpeed は「1 秒あたりに目標へ近づく強さ」として扱う。
        // WHY: 固定係数 Lerp では FPS によって追従感が変わるため、指数補間で dt に依存した
        //      補間率を作る。初回だけはシーン上の初期位置から遅れて寄る違和感を避けるためスナップする。
        const float safeDt = Max(dt, 0.0f);
        const float safeFollowSpeed = Max(followSpeed, 0.0f);
        const float followT = safeFollowSpeed <= EPSILON
            ? 1.0f
            : Clamp01(1.0f - Pow(0.001f, safeDt * safeFollowSpeed));
        const Vector3 camPos = m_hasCameraPosition
            ? Vector3::Lerp(transform->position, targetCamPos, followT)
            : targetCamPos;

        transform->localPosition = camPos;
        transform->position      = camPos;
        transform->localRotation = rotation;
        transform->rotation      = rotation;
        m_hasCameraPosition      = true;
    }

private:
    // targetTag に一致する GameObject をシーンから探し、追従対象として保持する。
    void FindTarget()
    {
        m_target = targetTag.empty() ? nullptr : scene.FindWithTag(targetTag);
    }

    GameObject* m_target = nullptr;
    bool m_hasCameraPosition = false;
};

} // namespace sandbox
