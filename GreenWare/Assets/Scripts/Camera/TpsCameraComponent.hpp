/// @file    TpsCameraComponent.hpp
/// @brief   プレイヤーを追う三人称カメラのスクリプト。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Camera/CameraLook.hpp>
#include <Scripts/Game/CameraFollowManagerComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using namespace fbzz::input;
using fbzz::Time;

namespace sandbox {

class TpsCameraComponent : public Script {
    FBZZ_SCRIPT(TpsCameraComponent)
    FBZZ_EXECUTE_ALWAYS()

    /// このスクリプトは「カメラの Transform を毎フレーム決める」ことしかしない。
    /// Camera の無い GameObject に付けると、何も映らないまま座標だけが動き続ける。
    FBZZ_REQUIRE_COMPONENT(CameraComponent)

public:
    FBZZ_FIELD(std::string, targetTag,        "Player", "対象のタグ")
    FBZZ_FIELD_RANGE(float, distance,          5.0f, "距離",         0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, height,            1.6f, "高さ",          -5.0f, 10.0f)
    FBZZ_FIELD(float, yaw,                     0.0f, "ヨー")
    FBZZ_FIELD(float, pitch,                  15.0f, "ピッチ")
    FBZZ_FIELD_RANGE(float, minPitch,         -20.0f, "ピッチ下限",       -90.0f,  0.0f)
    FBZZ_FIELD_RANGE(float, maxPitch,          65.0f, "ピッチ上限",         0.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, mouseSensitivity,   0.2f, "マウス感度", 0.01f, 5.0f)
    FBZZ_TOOLTIP("Option の「マウス感度」が既定値のときの旋回量。設定はこれに掛かる")
    FBZZ_FIELD(bool,  mouseOrbit,              true,  "マウス旋回")

    FBZZ_GROUP("ゲームパッド")
    /// デバイスごとに感度を分ける理由と、不感帯・応答カーブを Option (InputConfig) が
    /// 持つ理由は CameraLook.hpp に書いてある。FPS と同じ式でなければならない部分。
    FBZZ_FIELD(bool, padOrbit, true, "パッド旋回")
    FBZZ_FIELD_RANGE(float, padLookSpeed, 200.0f, "パッドの視点速度", 10.0f, 720.0f)
    FBZZ_TOOLTIP("Option の「スティック感度」が既定値のときの旋回速度 (度/秒)")
    FBZZ_FIELD(bool, padInvertY, false, "パッドの Y 反転")

    FBZZ_GROUP("パリィ後の構図")
    FBZZ_FIELD_RANGE(float, parryExtraDistance, 1.0f, "距離の余裕 [m]", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, parryFocusWeight, 0.25f, "相手へ注視点を寄せる割合", 0.0f, 0.45f)
    FBZZ_FIELD_RANGE(float, parryTurnSpeed, 45.0f, "相手への旋回上限 [度/秒]", 0.0f, 90.0f)
    FBZZ_GROUP("Follow")
    /// @note 縦と横は速さを分ける。跳躍は縦の変位だけ見せたいので横まで緩めると旋回に付いてこず酔う。回避はその逆で、1 本の速さでは両立しない。
    FBZZ_FIELD_RANGE(float, followSpeed,         10.0f, "Follow Speed",     0.0f, 50.0f)
    FBZZ_FIELD_RANGE(float, verticalFollowSpeed, 10.0f, "縦の追従",  0.0f, 50.0f)
    FBZZ_FIELD_RANGE(float, focusLagSeconds, 0.18f, "注視点の追従遅れ [秒]", 0.0f, 0.5f)
    FBZZ_TOOLTIP("移動でPlayerが画面中心からずれ、停止すると戻る。0で追加の遅れを無効化")

    FBZZ_GROUP("Follow Slack")
    /// たるみ 1.0 のときに使う追従速度。CameraFollowManagerComponent が 0..1 を配る。
    /// 追い付く速さそのものなので、絶対値で持つほうが調整しやすい (倍率だと
    /// 指数の肩に乗るため、0.03 のような直感の効かない数字になる)。
    FBZZ_FIELD_RANGE(float, loosenedFollowSpeed,         2.0f, "Loosened Follow",    0.0f, 20.0f)
    FBZZ_TOOLTIP("横に最大まで緩んだときの追従速度。低いほど画面内で大きく流れる")
    FBZZ_FIELD_RANGE(float, loosenedVerticalFollowSpeed, 1.4f, "Loosened Vertical",  0.0f, 20.0f)
    FBZZ_TOOLTIP("縦に最大まで緩んだときの追従速度。跳んだ高さが画面内の変位になる")
    FBZZ_FIELD_RANGE(float, maxSlackOffset, 2.2f, "Max Slack Offset", 0.0f, 10.0f)
    FBZZ_TOOLTIP("緩みで許す注視点からのずれ (m)。長い落下で対象が画面外へ出るのを防ぐ")

    /// @note 揺れは CameraShakeManagerComponent が生成・合成し、ここは合成済みオフセットを最終位置へ足すだけ (追従の作りを変えても揺れの調整はやり直しにならない)。
    /// @note 向きは Script フェーズ (入力のみ、物理を待たない) で先に確定し、位置は物理適用後の座標が要るため LateScript で追う。向きを遅らせるとレーザー等がクロスヘアから遅れて見える。
    void OnStart() override;
    void OnUpdate() override;
    void OnLateUpdate() override;

    /// @brief 演出カメラから操作を返してもらうときの引き継ぎ。
    /// @note 追従は前フレームの自分の位置 (m_unshakenPosition) から寄せるため、演出中に止まった記憶のまま有効化すると演出の画から遊びの画へ 1 フレームで飛ぶ。呼び出し側が現在の向きを渡すことで滑らかに戻せる。
    /// @note 向きの正本は yaw/pitch の 2 角 (transform.rotation は毎フレームそこから組み直す)。角を更新しないと位置だけ引き継ぎ、向きは演出前の方向へ瞬間で戻る。
    void ResumeFrom(const Vector3& position, const Quaternion& rotation);

private:
    /// yaw / pitch から今フレームの向きを組む。OnUpdate と OnLateUpdate が同じ式を使う。
    [[nodiscard]] Quaternion CurrentRotation() const;
    /// たるみ 0..1 を、密着側と最も緩い側の追従速度の間へ落とす。
    [[nodiscard]] static float BlendFollowSpeed(float tight, float loose, float slack01);
    /// 追従速度と dt から今フレームの補間率を出す。
    [[nodiscard]] static float FollowRate(float followSpeed, float dt);
    void FindTarget();
    /// 生ポインタを保持すると、対象が破棄された次のフレームに解放済みメモリを読む。
    /// EntityRef は generation まで Scene 側で検証するため、対象消滅を nullptr として扱える。
    EntityRef   m_target;
    float m_manualLookLeft = 0.0f;
    float m_parryBlend = 0.0f;
    Vector3 m_parryFocusOffset{};
    /// 視点操作の間だけカーソルを預かる要求。このカメラが消えれば自動で外れる。
    CursorRequest m_cursor;
    bool        m_hasCameraPosition = false;
    Vector3     m_unshakenPosition = Vector3::ZERO;
    Vector3     m_followFocus = Vector3::ZERO;
};

FBZZ_REFLECT(TpsCameraComponent)

inline void TpsCameraComponent::OnStart()
{
    m_manualLookLeft = m_parryBlend = 0.0f;
    m_parryFocusOffset = {};
    FindTarget();
    m_hasCameraPosition = false;
    m_unshakenPosition = Vector3::ZERO;

    /// @note 受聴点をカメラへ置く。TPS はプレイヤーでなくカメラが「聞いている場所」で、敵の左右が画面の見え方と一致する。Listener が無いと AudioSystem が距離減衰の基準を持てず、警告無しで 3D 音源が丸ごと鳴らなくなる。
    se::EnsureListener(scene);

    /// @note 視点操作は Locked (カーソルを毎フレーム中央へ戻し移動量のみ渡す) で回す。無いと画面端でカーソルが止まり振り向けなくなる。
    /// @note 直書きでなく Push で積む。メニュー/ポーズは絶対座標を使うため、閉じたときに自動で Locked へ戻せる。カメラが消えれば要求も消える。
    /// @note Play 中限定 (FBZZ_EXECUTE_ALWAYS で編集中も実行される)。無いとシーンを開いただけで Editor の OS カーソルが中央へ拘束される。
    if (app.IsPlaying())
        m_cursor = cursor.Push(CursorLockMode::Locked, false, CursorPriority::Camera);
}

inline Quaternion TpsCameraComponent::CurrentRotation() const
{
    const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(yaw));
    const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT, ToRad(pitch));
    return (yawRot * pitchRot).Normalized();
}

inline void TpsCameraComponent::ResumeFrom(const Vector3& position, const Quaternion& rotation)
{
    m_unshakenPosition  = position;
    m_hasCameraPosition = true;

    /// @note 前ベクトルから 2 つの角を復元する。CurrentRotation が yaw(Y) → pitch(X) の順で
    ///       組んでいるので、その逆順に解く。
    const Vector3 forward = (rotation * Vector3::FORWARD).NormalizedOr(Vector3::FORWARD);
    yaw   = ToDeg(std::atan2(forward.x, forward.z));
    /// @note 真上・真下を向いていると水平成分が消えて yaw が不定になる。pitch だけは
    ///       必ず取れるので、そちらは素直に asin で出す。
    pitch = Clamp(ToDeg(std::asin(Clamp(-forward.y, -1.0f, 1.0f))), minPitch, maxPitch);
    m_followFocus = position + CurrentRotation() * Vector3::FORWARD * distance;
}

inline void TpsCameraComponent::OnUpdate()
{
    if (!transform) return;

    /// @note ポーズ中は視点を受けない。マウスの移動量は時間に掛からないため timeScale=0 でも積まれ、メニュー操作でカーソルを動かした分だけ背後の視点が回ってしまう (パッドも同じ理由で実時間で積む)。
    const auto* timeManager = TimeManagerComponent::Instance();
    const bool  held = timeManager && timeManager->IsPaused();
    const float frameDt = held ? 0.0f : Max(Time::unscaledDeltaTime, 0.0f);
    m_manualLookLeft = Max(m_manualLookLeft - frameDt, 0.0f);

    /// @note Play 中限定。Input はエディタでも更新されるため、外すとビューポートでマウスを動かしただけで yaw/pitch がシーンの中身になる (編集中は Inspector の値が正本)。
    if (mouseOrbit && app.IsPlaying() && !held) {
        const Vector2 delta = cameralook::MouseDelta(input, mouseSensitivity);
        if (std::abs(delta.x) + std::abs(delta.y) > 0.01f) m_manualLookLeft = 0.8f;
        yaw   += delta.x;
        pitch  = Clamp(pitch + delta.y, minPitch, maxPitch);
    }

    if (padOrbit && app.IsPlaying() && !held) {
        /// @note 実時間 (unscaled) で積む。マウスは移動量そのものでヒットストップ中も動くため、スティックだけ scaled dt にすると止め中に効かずデバイスで挙動が食い違う。
        const float dt    = Max(Time::unscaledDeltaTime, 0.0f);
        const float speed = cameralook::PadLookSpeed(padLookSpeed);
        const Vector2 stick = cameralook::PadAxis(input);
        if (std::abs(stick.x) + std::abs(stick.y) > 0.01f) m_manualLookLeft = 0.8f;
        /// @note スティックの Y は上倒しが +1。画面の上を向くのは pitch が減る方向。
        const float pitchSign = padInvertY ? 1.0f : -1.0f;
        yaw   += stick.x * speed * dt;
        pitch  = Clamp(pitch + stick.y * pitchSign * speed * dt, minPitch, maxPitch);
    }

    if (app.IsPlaying() && !held && timeManager && timeManager->IsParryRush()
        && m_manualLookLeft <= 0.0f) {
        const auto* follow = CameraFollowManagerComponent::Instance();
        GameObject* opponent = follow ? follow->ParryOpponent() : nullptr;
        GameObject* player = m_target.Resolve(scene);
        if (opponent && opponent->activeInHierarchy() && player) {
            Vector3 toward = opponent->transform.worldPosition - player->transform.worldPosition;
            toward.y = 0.0f;
            if (toward.LengthSq() > 0.25f) {
                const float desired = ToDeg(std::atan2(toward.x, toward.z));
                const float turn = std::remainder(desired - yaw, 360.0f);
                yaw += Clamp(turn * (1.0f - std::exp(-frameDt * 4.0f)),
                             -parryTurnSpeed * frameDt, parryTurnSpeed * frameDt);
            }
            pitch = Lerp(pitch, Clamp(pitch, 10.0f, 28.0f), 1.0f - std::exp(-frameDt * 3.0f));
        }
    }

    /// @note 向きだけを先に置く。同じフレームの LateScript で照準がこれを読む。
    ///       位置はまだ前フレームのものだが、視線の起点が数 cm ずれても 40m 先の
    ///       到達点は同じだけしか動かない。画面上のクロスヘアと合うかを決めるのは向きの方。
    transform.rotation = CurrentRotation();
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

    /// @note 向きは OnUpdate で確定済み。ここで入力を読み直すと 1 フレームに 2 回転する。
    const Quaternion rotation = CurrentRotation();
    const auto* clock = TimeManagerComponent::Instance();
    if (clock && clock->IsPaused()) return;
    const float frameDt = clock && clock->IsPaused() ? 0.0f : Max(Time::unscaledDeltaTime, 0.0f);
    const auto* follow = CameraFollowManagerComponent::Instance();
    GameObject* opponent = follow ? follow->ParryOpponent() : nullptr;
    const bool framing = app.IsPlaying() && clock && clock->IsParryRush()
        && opponent && opponent->activeInHierarchy() && m_manualLookLeft <= 0.0f;
    const float blendRate = 1.0f - std::exp(-frameDt / 0.22f);
    m_parryBlend = Lerp(m_parryBlend, framing ? 1.0f : 0.0f, blendRate);
    Vector3 offset{};
    if (framing) {
        offset = (opponent->transform.worldPosition - target->transform.worldPosition) * parryFocusWeight;
        offset.y = Clamp(offset.y, -0.3f, 0.65f);
        const float span = offset.Length();
        if (span > 2.0f) offset = offset * (2.0f / span);
    }
    m_parryFocusOffset += (offset - m_parryFocusOffset) * blendRate;
    const float framedDistance = distance + parryExtraDistance * m_parryBlend;
    const Vector3 focus = target->transform.worldPosition + Vector3::UP * height + m_parryFocusOffset;
    const Vector3 targetCamPos = focus - (rotation * Vector3::FORWARD) * framedDistance;

    /// @note 実時間 (unscaled) で追従を回す。ヒットストップ中に縮んだ時間で回すと補間も遅くなり «当たった瞬間にカメラが引っ掛かる» (重い操作に見える)。揺れ・たるみは既に実時間なので追従だけ合わせる。
    const float safeDt = frameDt;

    /// @note 緩めたい要求が無ければ、たるみは 0 のまま = 従来どおりの密着追従になる。
    float horizontalSlack = 0.0f;
    float verticalSlack   = 0.0f;
    float fovOffset       = 0.0f;
    if (auto* follow = CameraFollowManagerComponent::Instance()) {
        horizontalSlack = follow->HorizontalSlack();
        verticalSlack   = follow->VerticalSlack();
        fovOffset       = follow->FovOffset();
    }

    /// @note 画角は Option の視野角を基準に張り出しを足して毎フレーム書き直す。基準自体が設定変更で動くため、1 度だけ書くと Option 変更が反映されない。
    if (auto* camera = scene.GetComponent<CameraComponent>())
        camera->fovY = GameSettingsComponent::GameOrDefault().fov
            + fovOffset * Lerp(1.0f, 0.35f, m_parryBlend);
    const auto focusRate = [&](float speed, float tightSpeed) {
        if (!app.IsPlaying() || focusLagSeconds <= EPSILON) return FollowRate(speed, safeDt);
        const float response = Max(focusLagSeconds, focusLagSeconds * Max(tightSpeed, EPSILON) / Max(speed, EPSILON));
        return 1.0f - std::exp(-safeDt / response);
    };
    const float horizontalRate = focusRate(
        BlendFollowSpeed(followSpeed, loosenedFollowSpeed, horizontalSlack), followSpeed);
    const float verticalRate = focusRate(
        BlendFollowSpeed(verticalFollowSpeed, loosenedVerticalFollowSpeed, verticalSlack), verticalFollowSpeed);

    Vector3 camPos = targetCamPos;
    if (m_hasCameraPosition && (focus - m_followFocus).LengthSq() < 400.0f) {
        /// @note 軸ごとに別の率で寄せる。1 本の Lerp では縦だけ遅らせられない。
        ///       旋回軌道は遅らせず、対象の移動だけを注視点で平滑化する。
        m_followFocus.x = Lerp(m_followFocus.x, focus.x, horizontalRate);
        m_followFocus.z = Lerp(m_followFocus.z, focus.z, horizontalRate);
        m_followFocus.y = Lerp(m_followFocus.y, focus.y, verticalRate);
        camPos = m_followFocus - (rotation * Vector3::FORWARD) * framedDistance;

        /// @note 緩みは「追い付かない」であって「置き去りにする」ではないため上限を設ける。落下が長いと縦の遅れが積み上がり対象が画面外に出るため、ずれの向きは保ったまま長さだけ抑える。
        const Vector3 slackOffset = camPos - targetCamPos;
        const float slackDistance = slackOffset.Length();
        const float limit         = Max(maxSlackOffset, 0.0f);
        if (slackDistance > limit && slackDistance > EPSILON)
            camPos = targetCamPos + slackOffset * (limit / slackDistance);
    }
    /// @note 前フレームの揺れを追従補間へ戻さない。戻すとランダムオフセットが積分されてカメラが漂う。
    m_unshakenPosition = camPos;
    m_followFocus = camPos + (rotation * Vector3::FORWARD) * framedDistance;

    /// @note 合成済みの揺れをカメラのローカル軸へ乗せる。生成も減衰もマネージャー側の仕事。
    if (auto* shake = CameraShakeManagerComponent::Instance())
        camPos += rotation * shake->CurrentOffset();

    transform.position      = camPos;
    transform.rotation      = rotation;
    m_hasCameraPosition     = true;
}

inline float TpsCameraComponent::BlendFollowSpeed(float tight, float loose, float slack01)
{
    return Lerp(Max(tight, 0.0f), Max(loose, 0.0f), Clamp01(slack01));
}

inline float TpsCameraComponent::FollowRate(float followSpeed, float dt)
{
    /// @note 0.001 は「1/followSpeed 秒でここまで残る」の意味。速さを秒に読み替えられる。
    return followSpeed <= EPSILON ? 1.0f : Clamp01(1.0f - Pow(0.001f, dt * followSpeed));
}

inline void TpsCameraComponent::FindTarget()
{
    m_hasCameraPosition = false;
    if (targetTag.empty()) {
        m_target = {};
        return;
    }
    if (GameObject* target = scene.FindWithTag(targetTag, true))
        m_target = EntityRef{ target->GetID() };
    else
        m_target = {};
}

} // namespace sandbox
