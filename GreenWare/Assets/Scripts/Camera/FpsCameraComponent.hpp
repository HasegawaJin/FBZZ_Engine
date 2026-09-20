/// @file    FpsCameraComponent.hpp
/// @brief   一人称視点。プレイヤーの目の位置にカメラを置き、視線をそのまま照準にする
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// @note Main Camera へは TPS/FPS どちらか片方だけ付ける (同じ Transform を奪い合う)。
/// @note TPS と統合しない。目の置き方が違い、切替フラグにすると半分の項目が死んだまま Inspector に並ぶ。旋回式のみ CameraLook.hpp で共有。
/// @note 追従のたるみは受け取らない。カメラ=頭そのものなので緩めると目が頭から外れる。CameraFollowManagerComponent からは画角の張り出しだけ読む。
/// @note 体を隠す。頭メッシュが near 面の内側に入り画面を覆うため。銃はボーンソケットの別 GameObject なので巻き添えにならない。
#pragma once

#include <Engine/Input/Input.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
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
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class FpsCameraComponent : public Script {
    FBZZ_SCRIPT(FpsCameraComponent)
    FBZZ_EXECUTE_ALWAYS()

    /// このスクリプトは「カメラの Transform を毎フレーム決める」ことしかしない。
    /// Camera の無い GameObject に付けると、何も映らないまま座標だけが動き続ける。
    FBZZ_REQUIRE_COMPONENT(CameraComponent)

public:
    FBZZ_FIELD(std::string, targetTag, "Player", "対象のタグ")

    FBZZ_GROUP("Eye")
    FBZZ_FIELD_RANGE(float, eyeHeight, 1.70f, "Eye Height", 0.0f, 5.0f)
    FBZZ_TOOLTIP("足元から目までの高さ (m)。Player のカプセルは上端 1.96m")
    /// @note 腰だめに構えた銃の銃口とビームの根元を画面に収めるため、数 cm 前へ出す。大きすぎると壁際でカメラが面を突き抜ける。
    FBZZ_FIELD_RANGE(float, eyeForward, 0.10f, "Eye Forward", -0.5f, 1.0f)
    FBZZ_TOOLTIP("目を視線方向へ出す量 (m)。大きくすると壁際でカメラが面を抜ける")
    FBZZ_FIELD(bool, hideBody, true, "Hide Body")
    FBZZ_TOOLTIP("遊んでいる間だけプレイヤーのメッシュを止める。影も一緒に消える")

    FBZZ_GROUP("表示")
    FBZZ_FIELD(float, yaw,   0.0f, "ヨー")
    FBZZ_FIELD(float, pitch, 0.0f, "ピッチ")
    /// @note TPS より広く取る (一人称では真上・真下も見える必要がある)。±90 手前で止めるのは極でヨーの意味が消え視界が回転するのを避けるため。
    FBZZ_FIELD_RANGE(float, minPitch, -85.0f, "ピッチ下限", -89.0f, 0.0f)
    FBZZ_FIELD_RANGE(float, maxPitch,  85.0f, "ピッチ上限",   0.0f, 89.0f)
    FBZZ_FIELD_RANGE(float, mouseSensitivity, 0.2f, "マウス感度", 0.01f, 5.0f)
    FBZZ_TOOLTIP("Option の「マウス感度」が既定値のときの旋回量。設定はこれに掛かる")
    FBZZ_FIELD(bool, mouseOrbit, true, "マウス旋回")

    FBZZ_GROUP("ゲームパッド")
    FBZZ_FIELD(bool, padOrbit, true, "パッド旋回")
    FBZZ_FIELD_RANGE(float, padLookSpeed, 200.0f, "パッドの視点速度", 10.0f, 720.0f)
    FBZZ_TOOLTIP("Option の「スティック感度」が既定値のときの旋回速度 (度/秒)")
    FBZZ_FIELD(bool, padInvertY, false, "パッドの Y 反転")

    FBZZ_GROUP("Ground Smoothing")
    /// @note 縦だけならす。段差・坂で接地点の Y が跳ぶため。横は遅れがそのまま「入力が重い」体感になるのでならさない。
    FBZZ_FIELD_RANGE(float, verticalFollowSpeed, 20.0f, "縦の追従", 0.0f, 60.0f)
    FBZZ_TOOLTIP("接地中に目の高さが追い付く速さ。0 でならさない")
    FBZZ_FIELD_RANGE(float, snapDistance, 1.5f, "Snap Distance", 0.1f, 20.0f)
    FBZZ_TOOLTIP("この距離以上ずれたらならさず飛ぶ。復活や転送で視界が引きずられない")

    FBZZ_GROUP("View Bob")
    /// @note 位相は時間でなく歩いた距離で進める。時間基準だと停止中も位相が進み、歩き出しの足が毎回変わる。
    FBZZ_FIELD_RANGE(float, bobAmplitude, 0.045f, "Bob Amplitude", 0.0f, 0.3f)
    FBZZ_TOOLTIP("上下の揺れ幅 (m)。0 で頭の揺れを切る")
    FBZZ_FIELD_RANGE(float, bobSway, 0.030f, "Bob Sway", 0.0f, 0.3f)
    FBZZ_TOOLTIP("左右の揺れ幅 (m)。上下の半分の周期で、2 歩で 1 往復する")
    FBZZ_FIELD_RANGE(float, bobDistance, 3.0f, "Bob Distance", 0.5f, 10.0f)
    FBZZ_TOOLTIP("1 往復に進む距離 (m)。上下はその半分ごと (1 歩ごと) に沈む")
    FBZZ_FIELD_RANGE(float, bobResponse, 8.0f, "Bob Response", 0.0f, 40.0f)
    FBZZ_TOOLTIP("歩き出しと止まりで揺れが立ち上がる / 消える速さ")

    /// @note 揺れ (被弾・着地・集束) は CameraShakeManagerComponent が生成・合成し、ここは合成済みオフセットを最終位置へ足すだけ (TPS と共通)。
    /// @note 向きは Script フェーズ (入力のみ、物理を待たない) で先に確定し、位置は物理適用後の座標が要るため LateScript で追う。照準等は同フレームで向きを読む。
    void OnStart() override;
    void OnUpdate() override;
    void OnLateUpdate() override;
    void OnDisable() override { ShowBody(); }
    void OnDestroy() override { ShowBody(); }

private:
    /// yaw / pitch から今フレームの向きを組む。OnUpdate と OnLateUpdate が同じ式を使う。
    [[nodiscard]] Quaternion CurrentRotation() const;
    /// 追従速度と dt から今フレームの補間率を出す。
    [[nodiscard]] static float FollowRate(float followSpeed, float dt);
    /// 対象が地に足を着けているか。CharacterController が無ければ接地とみなす。
    [[nodiscard]] bool IsGrounded(GameObject& target) const;
    /// 歩いた距離ぶん位相を進め、今フレームの頭の揺れをワールド座標で返す。
    [[nodiscard]] Vector3 TickBob(const Vector3& footPosition, const Vector3& right,
                                  bool grounded, float dt);
    /// hideBody と再生状態から、対象のメッシュを止める / 戻すを決める。
    void ApplyBodyVisibility(GameObject& target);
    void HideBody(GameObject& target);
    void ShowBody();
    [[nodiscard]] GameObject* ResolveTarget();
    void FindTarget();

    /// 生ポインタを保持すると、対象が破棄された次のフレームに解放済みメモリを読む。
    /// EntityRef は generation まで Scene 側で検証するため、対象消滅を nullptr として扱える。
    EntityRef m_target;
    /// 視点操作の間だけカーソルを預かる要求。このカメラが消えれば自動で外れる。
    CursorRequest m_cursor;

    bool  m_hasEye = false;
    float m_eyeY   = 0.0f;

    bool    m_hasBobOrigin = false;
    Vector3 m_bobOrigin    = Vector3::ZERO;
    float   m_bobPhase     = 0.0f;
    float   m_bobWeight    = 0.0f;

    /// 自分が止めたものだけを覚える。全部まとめて戻すと、別の理由で消えていた
    /// パーツ (演出中の点滅など) まで一人称をやめた瞬間に現れる。
    bool                   m_bodyHidden = false;
    std::vector<EntityRef> m_hiddenParts;
};

FBZZ_REFLECT(FpsCameraComponent)


inline void FpsCameraComponent::OnStart()
{
    FindTarget();
    m_hasEye       = false;
    m_hasBobOrigin = false;
    m_bobPhase     = 0.0f;
    m_bobWeight    = 0.0f;

    /// @note 受聴点をカメラへ置く (一人称は耳も目と同じ位置)。Listener が無いと AudioSystem が距離減衰の基準を持てず、警告無しで 3D 音源が丸ごと鳴らなくなる。
    se::EnsureListener(scene);

    /// @note 視点操作は Locked (カーソルを毎フレーム中央へ戻し移動量のみ渡す) で回す。無いと画面端でカーソルが止まり振り向けなくなる。
    /// @note 直書きでなく Push で積む。メニュー/ポーズは絶対座標を使うため、閉じたときに自動で Locked へ戻せる。カメラが消えれば要求も消える。
    /// @note Play 中限定 (FBZZ_EXECUTE_ALWAYS で編集中も実行される)。無いとシーンを開いただけで Editor の OS カーソルが中央へ拘束される。
    if (app.IsPlaying())
        m_cursor = cursor.Push(CursorLockMode::Locked, false, CursorPriority::Camera);
}

inline Quaternion FpsCameraComponent::CurrentRotation() const
{
    const Quaternion yawRot   = Quaternion::FromAxisAngle(Vector3::UP,    ToRad(yaw));
    const Quaternion pitchRot = Quaternion::FromAxisAngle(Vector3::RIGHT, ToRad(pitch));
    return (yawRot * pitchRot).Normalized();
}

inline void FpsCameraComponent::OnUpdate()
{
    if (!transform) return;

    /// @note Play 中限定。Input はエディタでも更新されるため、外すとビューポートでマウスを動かしただけで yaw/pitch がシーンの中身になる (編集中は Inspector の値が正本)。
    if (mouseOrbit && app.IsPlaying()) {
        const Vector2 delta = cameralook::MouseDelta(input, mouseSensitivity);
        yaw   += delta.x;
        pitch  = Clamp(pitch + delta.y, minPitch, maxPitch);
    }

    if (padOrbit) {
        /// @note 実時間 (unscaled) で積む。マウスは移動量そのものでヒットストップ中も動くため、スティックだけ scaled dt にすると止め中に効かずデバイスで挙動が食い違う。
        const float dt    = Max(Time::unscaledDeltaTime, 0.0f);
        const float speed = cameralook::PadLookSpeed(padLookSpeed);
        const Vector2 stick = cameralook::PadAxis(input);
        /// @note スティックの Y は上倒しが +1。画面の上を向くのは pitch が減る方向。
        const float pitchSign = padInvertY ? 1.0f : -1.0f;
        yaw   += stick.x * speed * dt;
        pitch  = Clamp(pitch + stick.y * pitchSign * speed * dt, minPitch, maxPitch);
    }

    /// @note 向きだけを先に置く。同じフレームの LateScript で照準がこれを読む。
    transform.rotation = CurrentRotation();
}

inline void FpsCameraComponent::OnLateUpdate()
{
    if (!transform) return;
    GameObject* target = ResolveTarget();
    if (!target) return;

    ApplyBodyVisibility(*target);

    /// @note 向きは OnUpdate で確定済み。ここで入力を読み直すと 1 フレームに 2 回転する。
    const Quaternion rotation = CurrentRotation();
    /// @note ピッチは右手軸まわりなので、右ベクトルは水平のまま。前は俯角のぶんだけ下を向くため、
    ///       目を前へ出すのには水平成分を使う (下を向いた瞬間に目が沈むのを避ける)。
    const Vector3 right   = rotation * Vector3::RIGHT;
    Vector3       forward = rotation * Vector3::FORWARD;
    forward.y = 0.0f;
    const Vector3 planarForward = forward.NormalizedOr(Vector3::ZERO);

    const Vector3 foot = target->transform.worldPosition;
    Vector3 eye = foot + Vector3::UP * eyeHeight + planarForward * eyeForward;

    /// @note 実時間で回す。縮んだ時間で追うとヒットストップ中に目の追従が遅くなり «当たると視点が引っ掛かる» に見える (TpsCameraComponent の safeDt と同じ理由)。
    const float safeDt   = Max(Time::unscaledDeltaTime, 0.0f);
    const bool  grounded = IsGrounded(*target);

    /// @note 空中は 1:1 で追う。跳躍と落下は縦の変位そのものが手応えなので、ならすと
    ///       跳んだ実感が消える。復活や転送の飛びは距離で見分けて素通しする。
    if (m_hasEye && grounded && Abs(eye.y - m_eyeY) < Max(snapDistance, EPSILON))
        eye.y = Lerp(m_eyeY, eye.y, FollowRate(verticalFollowSpeed, safeDt));
    m_eyeY   = eye.y;
    m_hasEye = true;

    eye += TickBob(foot, right, grounded, safeDt);

    /// @note 画角は Option の視野角を基準に張り出しを足して毎フレーム書き直す。基準自体が設定変更で動くため、1 度だけ書くと Option 変更が反映されない。
    float fovOffset = 0.0f;
    if (auto* follow = CameraFollowManagerComponent::Instance())
        fovOffset = follow->FovOffset();
    if (auto* camera = scene.GetComponent<CameraComponent>())
        camera->fovY = GameSettingsComponent::GameOrDefault().fov + fovOffset;

    /// @note 合成済みの揺れをカメラのローカル軸へ乗せる。生成も減衰もマネージャー側の仕事。
    if (auto* shake = CameraShakeManagerComponent::Instance())
        eye += rotation * shake->CurrentOffset();

    transform.position = eye;
    transform.rotation = rotation;
}

inline Vector3 FpsCameraComponent::TickBob(const Vector3& footPosition, const Vector3& right,
                                           bool grounded, float dt)
{
    const Vector3 previous = m_bobOrigin;
    m_bobOrigin = footPosition;
    if (!m_hasBobOrigin) {
        m_hasBobOrigin = true;
        return Vector3::ZERO;
    }

    Vector3 travel = footPosition - previous;
    travel.y = 0.0f;
    float distance = travel.Length();
    float speed    = dt > EPSILON ? distance / dt : 0.0f;
    /// @note 歩ける速さを超えていたら移動ではなく転送。位相も揺れも進めない。
    if (speed > 100.0f) {
        distance = 0.0f;
        speed    = 0.0f;
    }

    /// @note 止まりと着地で揺れを切るときは、そのまま振幅だけ落とす。位相を止めると
    ///       沈んだ位置で固まって見える。
    const float wanted = (grounded && speed > 0.2f) ? 1.0f : 0.0f;
    m_bobWeight = Lerp(m_bobWeight, wanted, FollowRate(bobResponse, dt));
    if (m_bobWeight < 0.01f) {
        /// @note 揺れが消えたところで位相を畳む。次の歩き出しが必ず同じ足から始まる。
        m_bobPhase  = 0.0f;
        m_bobWeight = 0.0f;
        return Vector3::ZERO;
    }

    m_bobPhase = std::fmod(m_bobPhase + (distance / Max(bobDistance, EPSILON)) * TWO_PI, TWO_PI);
    /// @note 上下は 1 歩ごと、左右は 2 歩で 1 往復。人の歩きは左右の足で重心が入れ替わるので、
    ///       横の周期は縦の半分になる。
    const float vertical = std::sin(m_bobPhase * 2.0f) * bobAmplitude * m_bobWeight;
    const float lateral  = std::sin(m_bobPhase) * bobSway * m_bobWeight;
    return right * lateral + Vector3::UP * vertical;
}

inline bool FpsCameraComponent::IsGrounded(GameObject& target) const
{
    auto* cc = scene.GetComponent<CharacterControllerComponent>(target);
    return cc ? cc->isGrounded : true;
}

inline void FpsCameraComponent::ApplyBodyVisibility(GameObject& target)
{
    /// @note 編集中は触らない。エディタでメッシュを消すと、その状態がシーンの中身になる。
    const bool wantHidden = hideBody && app.IsPlaying();
    if (wantHidden == m_bodyHidden) return;
    m_bodyHidden = wantHidden;

    if (wantHidden) HideBody(target);
    else            ShowBody();
}

inline void FpsCameraComponent::HideBody(GameObject& target)
{
    /// @note 直下の子だけを見る。パーツは FBX 分割のまま Player 直下に並ぶが、銃はボーンソケット (より深い階層) にぶら下がるため、部分木を丸ごと辿ると手にした銃まで消える。
    const int childCount = target.GetChildCount();
    for (int i = 0; i < childCount; ++i) {
        GameObject* part = target.GetChild(i);
        if (!part) continue;

        bool hid = false;
        if (auto* skinned = scene.GetComponent<SkinnedMeshRenderer>(part);
            skinned && skinned->enabled) {
            skinned->enabled = false;
            hid = true;
        }
        if (auto* solid = scene.GetComponent<MeshRenderer>(part); solid && solid->enabled) {
            solid->enabled = false;
            hid = true;
        }
        if (hid) m_hiddenParts.push_back(EntityRef{ part->GetID() });
    }
}

inline void FpsCameraComponent::ShowBody()
{
    for (const EntityRef& ref : m_hiddenParts) {
        GameObject* part = ref.Resolve(scene);
        if (!part) continue;
        if (auto* skinned = scene.GetComponent<SkinnedMeshRenderer>(part)) skinned->enabled = true;
        if (auto* solid = scene.GetComponent<MeshRenderer>(part))          solid->enabled = true;
    }
    m_hiddenParts.clear();
    m_bodyHidden = false;
}

inline float FpsCameraComponent::FollowRate(float followSpeed, float dt)
{
    /// @note 0.001 は「1/followSpeed 秒でここまで残る」の意味。速さを秒に読み替えられる。
    return followSpeed <= EPSILON ? 1.0f : Clamp01(1.0f - Pow(0.001f, dt * followSpeed));
}

inline GameObject* FpsCameraComponent::ResolveTarget()
{
    if (GameObject* target = m_target.Resolve(scene)) return target;
    FindTarget();
    return m_target.Resolve(scene);
}

inline void FpsCameraComponent::FindTarget()
{
    /// @note 対象が入れ替わったら、前の体を隠したままにしない。
    ShowBody();
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
