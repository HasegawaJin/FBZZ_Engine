// FBZZ Engine
// TpsCameraComponent.hpp | sandbox
#pragma once
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using fbzz::Time;

namespace sandbox {

class TpsCameraComponent : public Script {
    FBZZ_SCRIPT(TpsCameraComponent)

    // このスクリプトは「カメラの Transform を毎フレーム決める」ことしかしない。
    // Camera の無い GameObject に付けると、何も映らないまま座標だけが動き続ける。
    FBZZ_REQUIRE_COMPONENT(CameraComponent)

public:
    FBZZ_FIELD(std::string, targetTag,        "Player", "Target Tag")
    FBZZ_FIELD_RANGE(float, distance,          5.0f, "Distance",         0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, height,            1.6f, "Height",          -5.0f, 10.0f)
    FBZZ_FIELD(float, yaw,                     0.0f, "Yaw")
    FBZZ_FIELD(float, pitch,                  15.0f, "Pitch")
    FBZZ_FIELD_RANGE(float, minPitch,         -20.0f, "Min Pitch",       -90.0f,  0.0f)
    FBZZ_FIELD_RANGE(float, maxPitch,          65.0f, "Max Pitch",         0.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, mouseSensitivity,   0.2f, "Mouse Sensitivity", 0.01f, 5.0f)
    FBZZ_FIELD(bool,  mouseOrbit,              true,  "Mouse Orbit")
    FBZZ_FIELD_RANGE(float, followSpeed,       10.0f, "Follow Speed",      0.0f, 50.0f)

    FBZZ_GROUP("Shake")
    FBZZ_FIELD_RANGE(float, shakeFrequency, 32.0f, "Shake Frequency", 1.0f, 80.0f)

    // 衝突・被弾側から同じ TPS カメラへ揺れを要求する。強い要求は弱い揺れを上書きする。
    void StartShake(float amplitude, float duration);

    void OnStart() override;
    void OnLateUpdate() override;

private:
    void FindTarget();
    // 生ポインタを保持すると、対象が破棄された次のフレームに解放済みメモリを読む。
    // EntityRef は generation まで Scene 側で検証するため、対象消滅を nullptr として扱える。
    EntityRef   m_target;
    bool        m_hasCameraPosition = false;
    Vector3     m_unshakenPosition = Vector3::ZERO;
    float       m_shakeAmplitude = 0.0f;
    float       m_shakeDuration = 0.0f;
    float       m_shakeRemaining = 0.0f;
};

FBZZ_REFLECT(TpsCameraComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void TpsCameraComponent::OnStart()
{
    FindTarget();
    m_hasCameraPosition = false;
    m_unshakenPosition = Vector3::ZERO;
    m_shakeAmplitude = 0.0f;
    m_shakeDuration = 0.0f;
    m_shakeRemaining = 0.0f;
}

inline void TpsCameraComponent::StartShake(float amplitude, float duration)
{
    if (amplitude <= 0.0f || duration <= 0.0f) return;
    if (amplitude >= m_shakeAmplitude || m_shakeRemaining <= 0.0f) {
        m_shakeAmplitude = amplitude;
        m_shakeDuration = duration;
    }
    m_shakeRemaining = Max(m_shakeRemaining, duration);
}

inline void TpsCameraComponent::OnLateUpdate()
{
    if (!transform) return;
    GameObject* target = m_target.Resolve(scene);
    if (!target) {
        FindTarget();
        target = m_target.Resolve(scene);
    }
    if (!target) return;

    if (mouseOrbit) {
        const Vector2 delta = input.GetMouseDelta();
        yaw   += delta.x * mouseSensitivity;
        pitch  = Clamp(pitch + delta.y * mouseSensitivity, minPitch, maxPitch);
    }

    const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(yaw));
    const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT, ToRad(pitch));
    const Quaternion rotation = (yawRot * pitchRot).Normalized();
    // 追従対象は固定物理の確定姿勢ではなく、描画と同じ補間済み姿勢を見る。
    // WHY: カメラと Player が異なる時間軸を参照すると、距離が fixed step ごとに伸縮するため。
    const Vector3 focus =
        target->transform.presentationWorldPosition + Vector3::UP * height;
    const Vector3 targetCamPos = focus - (rotation * Vector3::FORWARD) * distance;

    // WHY: 指数補間で dt に依存した補間率を計算。初回のみスナップして位置ずれを防ぐ。
    const float safeDt          = Max(Time::deltaTime, 0.0f);
    const float safeFollowSpeed = Max(followSpeed, 0.0f);
    const float followT = safeFollowSpeed <= EPSILON
        ? 1.0f
        : Clamp01(1.0f - Pow(0.001f, safeDt * safeFollowSpeed));
    Vector3 camPos = m_hasCameraPosition
        ? Vector3::Lerp(m_unshakenPosition, targetCamPos, followT)
        : targetCamPos;
    // 前フレームの揺れを追従補間へ戻さない。戻すとランダムオフセットが積分されてカメラが漂う。
    m_unshakenPosition = camPos;

    // ヒットストップ中も揺れを進めるため unscaled 時間を使う。停止中に完全静止すると
    // 「ドンッ」の最初のフレームが無反応に見え、停止解除後に遅れて揺れてしまう。
    if (m_shakeRemaining > 0.0f && m_shakeDuration > EPSILON) {
        const float strength = Clamp01(m_shakeRemaining / m_shakeDuration);
        const float phase = Time::unscaledTime * shakeFrequency;
        const Vector3 localOffset{
            std::sin(phase * 1.17f),
            std::cos(phase * 1.73f) * 0.65f,
            0.0f
        };
        camPos += rotation * localOffset * (m_shakeAmplitude * strength * strength);
        m_shakeRemaining = Max(0.0f, m_shakeRemaining - time.UnscaledDeltaTime());
        if (m_shakeRemaining <= 0.0f) m_shakeAmplitude = 0.0f;
    }

    transform.position      = camPos;
    transform.rotation      = rotation;
    m_hasCameraPosition     = true;
}

inline void TpsCameraComponent::FindTarget()
{
    if (targetTag.empty()) {
        m_target = {};
        return;
    }
    if (GameObject* target = scene.FindWithTag(targetTag))
        m_target = EntityRef{ target->GetID() };
    else
        m_target = {};
}

} // namespace sandbox
