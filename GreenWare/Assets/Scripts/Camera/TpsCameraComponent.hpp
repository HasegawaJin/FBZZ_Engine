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

    // このスクリプトは「カメラの Transform を毎フレーム決める」ことしかしない。
    // Camera の無い GameObject に付けると、何も映らないまま座標だけが動き続ける。
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
    // デバイスごとに感度を分ける理由と、不感帯・応答カーブを Option (InputConfig) が
    // 持つ理由は CameraLook.hpp に書いてある。FPS と同じ式でなければならない部分。
    FBZZ_FIELD(bool, padOrbit, true, "パッド旋回")
    FBZZ_FIELD_RANGE(float, padLookSpeed, 200.0f, "パッドの視点速度", 10.0f, 720.0f)
    FBZZ_TOOLTIP("Option の「スティック感度」が既定値のときの旋回速度 (度/秒)")
    FBZZ_FIELD(bool, padInvertY, false, "パッドの Y 反転")

    FBZZ_GROUP("Follow")
    // WHY 縦と横を分けるか: 跳躍で見せたいのは縦の変位だけで、横まで一緒に緩めると
    //     旋回に付いてこなくなって酔う。回避で見せたいのはその逆。
    //     1 本の速さしか無いと、どちらかを諦めることになる。
    FBZZ_FIELD_RANGE(float, followSpeed,         10.0f, "Follow Speed",     0.0f, 50.0f)
    FBZZ_FIELD_RANGE(float, verticalFollowSpeed, 10.0f, "縦の追従",  0.0f, 50.0f)
    FBZZ_FIELD_RANGE(float, focusLagSeconds, 0.18f, "注視点の追従遅れ [秒]", 0.0f, 0.5f)
    FBZZ_TOOLTIP("移動でPlayerが画面中心からずれ、停止すると戻る。0で追加の遅れを無効化")

    FBZZ_GROUP("Follow Slack")
    // たるみ 1.0 のときに使う追従速度。CameraFollowManagerComponent が 0..1 を配る。
    // 追い付く速さそのものなので、絶対値で持つほうが調整しやすい (倍率だと
    // 指数の肩に乗るため、0.03 のような直感の効かない数字になる)。
    FBZZ_FIELD_RANGE(float, loosenedFollowSpeed,         2.0f, "Loosened Follow",    0.0f, 20.0f)
    FBZZ_TOOLTIP("横に最大まで緩んだときの追従速度。低いほど画面内で大きく流れる")
    FBZZ_FIELD_RANGE(float, loosenedVerticalFollowSpeed, 1.4f, "Loosened Vertical",  0.0f, 20.0f)
    FBZZ_TOOLTIP("縦に最大まで緩んだときの追従速度。跳んだ高さが画面内の変位になる")
    FBZZ_FIELD_RANGE(float, maxSlackOffset, 2.2f, "Max Slack Offset", 0.0f, 10.0f)
    FBZZ_TOOLTIP("緩みで許す注視点からのずれ (m)。長い落下で対象が画面外へ出るのを防ぐ")

    // WHY 揺れを持たないか: 揺れの生成と合成は CameraShakeManagerComponent の担当。
    //     ここは合成済みのオフセットを受け取って最終位置へ足すだけにする。
    //     こうしておくと、追従の作りを変えても揺れの調整はやり直しにならない。

    // WHY 向きと位置でフェーズを分けるか:
    //   照準・レーザー・上半身のエイム姿勢は、すべて「カメラが今どこを向いているか」から
    //   引かれる。向きを LateScript で決めると、それらが読めるのは次のフレームになり、
    //   マウスを素早く振ったときにレーザーだけがクロスヘアから遅れて付いてくる。
    //   向きはマウス入力だけで決まり物理を待つ必要が無いので、Script フェーズで先に確定させる。
    //   位置は物理適用後のプレイヤー座標が要るため、これまでどおり LateScript で追う。
    void OnStart() override;
    void OnUpdate() override;
    void OnLateUpdate() override;

    /// 演出カメラから操作を返してもらうときの引き継ぎ。
    ///
    /// WHY 必要か: 追従は «前フレームの自分の位置» から寄せる (m_unshakenPosition)。
    ///     演出がカメラを別の場所へ持って行っている間もその記憶は止まったままなので、
    ///     ただ有効へ戻すと «演出の画から遊びの画へ 1 フレームで飛ぶ» ことになる。
    ///     返す側が «今どこを向いて居るか» を教えれば、そこから滑らかに戻せる。
    ///
    /// WHY 向きを yaw / pitch へ戻すか: 向きの正本は入力で回している 2 つの角で、
    ///     transform.rotation は毎フレームそこから組み直される。角を更新しないと、
    ///     位置だけ引き継いで向きは «演出前に見ていた方向» へ瞬間で戻る。
    void ResumeFrom(const Vector3& position, const Quaternion& rotation);

private:
    /// yaw / pitch から今フレームの向きを組む。OnUpdate と OnLateUpdate が同じ式を使う。
    [[nodiscard]] Quaternion CurrentRotation() const;
    /// たるみ 0..1 を、密着側と最も緩い側の追従速度の間へ落とす。
    [[nodiscard]] static float BlendFollowSpeed(float tight, float loose, float slack01);
    /// 追従速度と dt から今フレームの補間率を出す。
    [[nodiscard]] static float FollowRate(float followSpeed, float dt);
    void FindTarget();
    // 生ポインタを保持すると、対象が破棄された次のフレームに解放済みメモリを読む。
    // EntityRef は generation まで Scene 側で検証するため、対象消滅を nullptr として扱える。
    EntityRef   m_target;
    // 視点操作の間だけカーソルを預かる要求。このカメラが消えれば自動で外れる。
    CursorRequest m_cursor;
    bool        m_hasCameraPosition = false;
    Vector3     m_unshakenPosition = Vector3::ZERO;
    Vector3     m_followFocus = Vector3::ZERO;
};

FBZZ_REFLECT(TpsCameraComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────
inline void TpsCameraComponent::OnStart()
{
    FindTarget();
    m_hasCameraPosition = false;
    m_unshakenPosition = Vector3::ZERO;

    // 受聴点をカメラへ置く。TPS なのでプレイヤーではなくカメラが「聞いている場所」で、
    // 敵の左右も画面の見え方と一致する。
    //
    // WHY ここで足すか: Listener がシーンに 1 つも無いと AudioSystem は距離減衰の
    //     基準を持てず、3D 音源が丸ごと鳴らない。しかも警告は出ないので、
    //     症状は「敵の音だけ無音」という形でしか現れない。
    se::EnsureListener(scene);

    // 視点はマウスの «移動量» で回す。Locked は毎フレーム OS カーソルを画面中央へ戻して
    // 移動量だけを渡すモードで、これが無いとカーソルが画面端に着いた時点で振り向けなくなる。
    //
    // WHY 直書きでなく Push か: 同じゲームでもメニューやポーズは «絶対座標» で
    //     カーソルを動かす。要求として積んでおけば、上に重ねたメニューが閉じたときに
    //     視点操作の Locked へ自動で戻る。カメラが消えれば要求も一緒に消える。
    //
    // WHY Play 中だけか: このスクリプトは FBZZ_EXECUTE_ALWAYS() で編集中も走る。
    //     ガードが無いと、このカメラが居るシーンを開いただけで Editor の OS カーソルが
    //     消え、ウィンドウ中央へ拘束される。
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

    // 前ベクトルから 2 つの角を復元する。CurrentRotation が yaw(Y) → pitch(X) の順で
    // 組んでいるので、その逆順に解く。
    const Vector3 forward = (rotation * Vector3::FORWARD).NormalizedOr(Vector3::FORWARD);
    yaw   = ToDeg(std::atan2(forward.x, forward.z));
    // 真上・真下を向いていると水平成分が消えて yaw が不定になる。pitch だけは
    // 必ず取れるので、そちらは素直に asin で出す。
    pitch = Clamp(ToDeg(std::asin(Clamp(-forward.y, -1.0f, 1.0f))), minPitch, maxPitch);
    m_followFocus = position + CurrentRotation() * Vector3::FORWARD * distance;
}

inline void TpsCameraComponent::OnUpdate()
{
    if (!transform) return;

    // WHY 編集中を除くか: Input はエディタでも更新され続けているため、囲まないと
    //     ビューポート上でマウスを動かしただけで yaw / pitch が積まれ、その値が
    //     シーンの中身になる。編集中の向きは Inspector に置いた値が正本。
    if (mouseOrbit && app.IsPlaying()) {
        const Vector2 delta = cameralook::MouseDelta(input, mouseSensitivity);
        yaw   += delta.x;
        pitch  = Clamp(pitch + delta.y, minPitch, maxPitch);
    }

    if (padOrbit) {
        // WHY 実時間で積むか: マウスは移動量そのものなのでヒットストップ中も動く。
        //     スティックだけスケール後の dt で積むと、止めが掛かった瞬間に
        //     視点だけ操作を受け付けなくなり、デバイスで挙動が食い違う。
        const float dt    = Max(Time::unscaledDeltaTime, 0.0f);
        const float speed = cameralook::PadLookSpeed(padLookSpeed);
        const Vector2 stick = cameralook::PadAxis(input);
        // スティックの Y は上倒しが +1。画面の上を向くのは pitch が減る方向。
        const float pitchSign = padInvertY ? 1.0f : -1.0f;
        yaw   += stick.x * speed * dt;
        pitch  = Clamp(pitch + stick.y * pitchSign * speed * dt, minPitch, maxPitch);
    }

    // 向きだけを先に置く。同じフレームの LateScript で照準がこれを読む。
    // 位置はまだ前フレームのものだが、視線の起点が数 cm ずれても 40m 先の
    // 到達点は同じだけしか動かない。画面上のクロスヘアと合うかを決めるのは向きの方。
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

    // 向きは OnUpdate で確定済み。ここで入力を読み直すと 1 フレームに 2 回転する。
    const Quaternion rotation = CurrentRotation();
    const Vector3 focus = target->transform.worldPosition + Vector3::UP * height;
    const Vector3 targetCamPos = focus - (rotation * Vector3::FORWARD) * distance;

    // WHY: 指数補間で dt に依存した補間率を計算。初回のみスナップして位置ずれを防ぐ。
    //
    // WHY 実時間 (Unscaled) で回すか (2026-09-11):
    //   ヒットストップは世界の時間を 5% まで落とす。縮んだ時間で追従を回すと、
    //   止まっている 0.09 秒のあいだカメラの補間も 20 分の 1 になり、
    //   «当たった瞬間にカメラが引っ掛かる» ── 止めの手応えではなく «重い操作» に
    //   見える。カメラは盤面の一員ではなく «見ている目» なので、世界が止まっても
    //   目は動いていてよい。揺れ (CameraShakeManager) と たるみ
    //   (CameraFollowManager) が既に実時間で回っているので、追従だけが
    //   縮んだ時間に取り残されていた形になる。
    const float safeDt = Max(Time::unscaledDeltaTime, 0.0f);

    // 緩めたい要求が無ければ、たるみは 0 のまま = 従来どおりの密着追従になる。
    float horizontalSlack = 0.0f;
    float verticalSlack   = 0.0f;
    float fovOffset       = 0.0f;
    if (auto* follow = CameraFollowManagerComponent::Instance()) {
        horizontalSlack = follow->HorizontalSlack();
        verticalSlack   = follow->VerticalSlack();
        fovOffset       = follow->FovOffset();
    }

    // 画角は Option の「視野角」が基準で、点火・集束の張り出しをそこへ足す。
    // WHY 毎フレーム書くか: 基準そのものが設定変更で動く。1 度だけ書くと、
    //     Option で視野角を変えても遊びに戻るまで反映されない。
    if (auto* camera = scene.GetComponent<CameraComponent>())
        camera->fovY = GameSettingsComponent::GameOrDefault().fov + fovOffset;
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
        // 軸ごとに別の率で寄せる。1 本の Lerp では縦だけ遅らせられない。
        // 旋回軌道は遅らせず、対象の移動だけを注視点で平滑化する。
        m_followFocus.x = Lerp(m_followFocus.x, focus.x, horizontalRate);
        m_followFocus.z = Lerp(m_followFocus.z, focus.z, horizontalRate);
        m_followFocus.y = Lerp(m_followFocus.y, focus.y, verticalRate);
        camPos = m_followFocus - (rotation * Vector3::FORWARD) * distance;

        // WHY 上限を設けるか: 緩みは「追い付かない」であって「置き去りにする」ではない。
        //     落下が長いと縦の遅れが積み上がり、対象が画面から出てしまう。
        //     ずれの向きは保ったまま長さだけ抑える。
        const Vector3 slackOffset = camPos - targetCamPos;
        const float slackDistance = slackOffset.Length();
        const float limit         = Max(maxSlackOffset, 0.0f);
        if (slackDistance > limit && slackDistance > EPSILON)
            camPos = targetCamPos + slackOffset * (limit / slackDistance);
    }
    // 前フレームの揺れを追従補間へ戻さない。戻すとランダムオフセットが積分されてカメラが漂う。
    m_unshakenPosition = camPos;
    m_followFocus = camPos + (rotation * Vector3::FORWARD) * distance;

    // 合成済みの揺れをカメラのローカル軸へ乗せる。生成も減衰もマネージャー側の仕事。
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
    // 0.001 は「1/followSpeed 秒でここまで残る」の意味。速さを秒に読み替えられる。
    return followSpeed <= EPSILON ? 1.0f : Clamp01(1.0f - Pow(0.001f, dt * followSpeed));
}

inline void TpsCameraComponent::FindTarget()
{
    m_hasCameraPosition = false;
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
