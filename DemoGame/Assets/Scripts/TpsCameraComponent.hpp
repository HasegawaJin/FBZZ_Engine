// FBZZ Engine
// TpsCameraComponent.hpp | sandbox
#pragma once
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using fbzz::Time;

namespace sandbox {

class TpsCameraComponent : public Script {
    FBZZ_SCRIPT(TpsCameraComponent)

public:
    // 追従対象。Inspector で直接アサインでき、未アサイン時はタグで探索する。
    FBZZ_REF(GameObject, target, "Target")
    FBZZ_FIELD(std::string, targetTag,        "Player", "Target Tag")
    FBZZ_FIELD_RANGE(float, distance,          5.0f,  "", 0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, height,            1.6f,  "", -5.0f, 10.0f)
    FBZZ_FIELD(float, yaw,                      0.0f, "")
    FBZZ_FIELD(float, pitch,                   15.0f, "")
    FBZZ_FIELD_RANGE(float, minPitch,         -20.0f, "", -90.0f,  0.0f)
    FBZZ_FIELD_RANGE(float, maxPitch,          65.0f, "",   0.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, mouseSensitivity,   0.2f, "", 0.01f, 5.0f)
    FBZZ_FIELD(bool,  mouseOrbit,              true,  "")
    FBZZ_FIELD_RANGE(float, followSpeed,       10.0f, "",  0.0f, 50.0f)

    FBZZ_GROUP("Camera Shake")
    // 剣が命中した瞬間にカメラを揺らして打撃の手応えを出す「トラウマ」式シェイク。
    // WeaponHitbox が命中時に AddTrauma() を呼び、trauma を [0,1] へ加算する。
    FBZZ_FIELD(bool, enableShake, true, "Enable Shake")
    // 位置の最大ずれ幅 (m)。trauma=1 のとき shakePositionStrength だけカメラが上下左右へ振れる。
    FBZZ_FIELD_RANGE(float, shakePositionStrength, 0.16f, "Position Strength", 0.0f, 1.0f)
    // ロール (前方軸まわり) を主体とした最大回転ずれ (度)。斬撃の手応えはロールが効く。
    FBZZ_FIELD_RANGE(float, shakeRotationStrength, 2.2f,  "Rotation Strength", 0.0f, 15.0f)
    // 揺れの速さ。高いほど鋭く細かく振動する。
    FBZZ_FIELD_RANGE(float, shakeFrequency,        26.0f, "Frequency",         1.0f, 60.0f)
    // trauma の毎秒減衰量。大きいほど揺れが速く収まる。
    FBZZ_FIELD_RANGE(float, shakeTraumaDecay,      1.8f,  "Trauma Decay",      0.1f, 10.0f)

    void OnStart() override;
    void OnLateUpdate() override;

    // 外部スクリプト (WeaponHitbox) が命中時に呼ぶ。trauma を加算して揺れを起こす。
    void AddTrauma(float amount);

private:
    GameObject* ResolveTarget();
    // RNG/Perlin に依存せず、周波数の異なる正弦波の和で [-1,1] の滑らかな擬似ノイズを作る。
    [[nodiscard]] static float ShakeNoise(float t, float seed);

    GameObject* m_target            = nullptr;
    bool        m_hasCameraPosition = false;
    // シェイク前の追従位置。揺れを毎フレームの補間元へ混ぜないよう、基準位置を別管理する。
    Vector3     m_basePosition      = Vector3::ZERO;
    // 現在の揺れ強度 [0,1]。命中で加算され、毎フレーム減衰する。
    float       m_trauma            = 0.0f;
};

FBZZ_REFLECT(TpsCameraComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void TpsCameraComponent::OnStart()
{
    m_target = ResolveTarget();
    m_hasCameraPosition = false;
}

inline void TpsCameraComponent::OnLateUpdate()
{
    if (!transform) return;
    if (!m_target || !m_target->IsValid()) m_target = ResolveTarget();
    if (!m_target) return;

    if (mouseOrbit) {
        const Vector2 delta = input.GetMouseDelta();
        yaw   += delta.x * mouseSensitivity;
        pitch  = Clamp(pitch + delta.y * mouseSensitivity, minPitch, maxPitch);
    }

    const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(yaw));
    const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT, ToRad(pitch));
    const Quaternion rotation = (yawRot * pitchRot).Normalized();
    const Vector3    focus    = m_target->transform.worldPosition + Vector3::UP * height;
    const Vector3    targetCamPos = focus - (rotation * Vector3::FORWARD) * distance;

    // WHY: 指数補間で dt に依存した補間率を計算。初回のみスナップして位置ずれを防ぐ。
    const float safeDt          = Max(Time::deltaTime, 0.0f);
    const float safeFollowSpeed = Max(followSpeed, 0.0f);
    const float followT = safeFollowSpeed <= EPSILON
        ? 1.0f
        : Clamp01(1.0f - Pow(0.001f, safeDt * safeFollowSpeed));
    // WHY: 補間元はシェイク込みの transform ではなく前フレームの「基準位置」を使う。
    //      揺れを補間へ巻き込むと、揺れが追従位置に残留して波打って見えるため分離する。
    const Vector3 camPos = m_hasCameraPosition
        ? Vector3::Lerp(m_basePosition, targetCamPos, followT)
        : targetCamPos;
    m_basePosition      = camPos;
    m_hasCameraPosition = true;

    // ── ヒットシェイク ───────────────────────────────────────────────────────
    // 基準の追従結果 (camPos / rotation) に、trauma に応じた揺れを上乗せする。
    Vector3    finalPos      = camPos;
    Quaternion finalRotation = rotation;
    if (enableShake && m_trauma > 0.0f) {
        // WHY: 実時間で減衰させることで、ヒットストップ (timeScale=0) 中でも揺れが
        //      止まらず「凍りついた一瞬が震える」打撃感になる。
        const float dtu = Max(time.UnscaledDeltaTime(), 0.0f);
        m_trauma = Max(0.0f, m_trauma - shakeTraumaDecay * dtu);

        // WHY: trauma² で減衰曲線を非線形にし、強い衝撃は鋭く・余韻は穏やかに収める (Eiserloh 方式)。
        const float shake = m_trauma * m_trauma;
        const float phase = time.UnscaledTime() * shakeFrequency;
        const float nx = ShakeNoise(phase, 0.0f);
        const float ny = ShakeNoise(phase, 17.3f);
        const float nr = ShakeNoise(phase, 41.7f);

        // 位置: カメラのローカル右・上方向へずらす (奥行きは動かさず構図を保つ)。
        const Vector3 right = rotation * Vector3::RIGHT;
        const Vector3 up    = rotation * Vector3::UP;
        finalPos += (right * nx + up * ny) * (shakePositionStrength * shake);

        // 回転: 前方軸まわりのロールを乗せ、打撃の衝撃を視界の傾きで表現する。
        const float roll = ToRad(shakeRotationStrength * shake * nr);
        finalRotation = (Quaternion::FromAxisAngle(rotation * Vector3::FORWARD, roll) * rotation).Normalized();
    }

    transform.position = finalPos;
    transform.rotation = finalRotation;
}

inline void TpsCameraComponent::AddTrauma(float amount)
{
    if (amount <= 0.0f) return;
    // 連続ヒットでは加算して揺れを強め、上限 1.0 でクランプする。
    m_trauma = Clamp01(m_trauma + amount);
}

inline float TpsCameraComponent::ShakeNoise(float t, float seed)
{
    // 周波数比 2.13 の 2 波を 0.6 : 0.4 で重ねる。和の振幅は 1.0 なので値域は [-1,1]。
    // 位相に seed を与えることで軸ごとに無相関な揺れを生む。
    return std::sin(t + seed) * 0.6f + std::sin(t * 2.13f + seed * 1.7f) * 0.4f;
}

inline GameObject* TpsCameraComponent::ResolveTarget()
{
    // 1) Inspector でアサインされた参照を最優先。2) なければタグで探索。
    if (GameObject* assigned = target.object())
        return assigned;
    return targetTag.empty() ? nullptr : scene.FindWithTag(targetTag);
}

} // namespace sandbox
