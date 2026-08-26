/// @file FpsCameraComponent.hpp
/// @brief 一人称視点。プレイヤーの目の位置にカメラを置き、視線をそのまま照準にする
/// @author Hasegawa Jin
/// @date 2026-08-26
///
/// 使い方: Main Camera から TpsCameraComponent を外し、これを付ける。
///         両方を付けると 2 つのスクリプトが同じ Transform を奪い合う。
///
/// WHY TPS と 1 つのスクリプトにまとめないか:
///   目の置き方が「後ろから追う」から「頭に乗せる」へ変わると、TPS 側の作り
///   (距離・注視点・追従のたるみ) はすべて意味を失う。切り替えフラグ 1 つで両方を
///   持たせると、どちらの視点でも半分の項目が死んだまま Inspector に並ぶ。
///   視点の回し方だけは両者で同じでなければならないので、そこは CameraLook.hpp で共有する。
///
/// WHY 追従のたるみを受け取らないか:
///   TPS のたるみは「カメラが遅れて付いてくると、画面の中でプレイヤーが動いて見える」
///   ための仕掛け。一人称ではカメラが頭そのものなので、緩めると自分の頭から目が
///   外れて後ろへ流れる。CameraFollowManagerComponent からは画角の張り出しだけを読む。
///
/// WHY 体を隠すか:
///   目は頭の内側にある。頭のメッシュが near 面の内側へ入るため、隠さないと画面が
///   自分の頭の裏地で埋まる。銃はボーンのソケットにぶら下がる別 GameObject なので
///   巻き添えにはならず、一人称でもそのまま手元に見える。
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

    // このスクリプトは「カメラの Transform を毎フレーム決める」ことしかしない。
    // Camera の無い GameObject に付けると、何も映らないまま座標だけが動き続ける。
    FBZZ_REQUIRE_COMPONENT(CameraComponent)

public:
    FBZZ_FIELD(std::string, targetTag, "Player", "Target Tag")

    FBZZ_GROUP("Eye")
    FBZZ_FIELD_RANGE(float, eyeHeight, 1.70f, "Eye Height", 0.0f, 5.0f)
    FBZZ_TOOLTIP("足元から目までの高さ (m)。Player のカプセルは上端 1.96m")
    // WHY 前へ出す量を持つか: 目の位置が首の真上のままだと、腰だめに構えた銃が
    //     画面のかなり下に来る。数 cm 前へ出すと銃口とビームの根元が画面へ収まる。
    //     大きくすると壁へ寄ったときにカメラが面を突き抜けるので、cm の単位で足りる。
    FBZZ_FIELD_RANGE(float, eyeForward, 0.10f, "Eye Forward", -0.5f, 1.0f)
    FBZZ_TOOLTIP("目を視線方向へ出す量 (m)。大きくすると壁際でカメラが面を抜ける")
    FBZZ_FIELD(bool, hideBody, true, "Hide Body")
    FBZZ_TOOLTIP("遊んでいる間だけプレイヤーのメッシュを止める。影も一緒に消える")

    FBZZ_GROUP("View")
    FBZZ_FIELD(float, yaw,   0.0f, "Yaw")
    FBZZ_FIELD(float, pitch, 0.0f, "Pitch")
    // WHY TPS より広く取るか: 一人称では真上と真下が「見えない方向」になってはいけない。
    //     真上 (±90) 手前で止めるのは、極でヨーの意味が消えて視界が回るのを避けるため。
    FBZZ_FIELD_RANGE(float, minPitch, -85.0f, "Min Pitch", -89.0f, 0.0f)
    FBZZ_FIELD_RANGE(float, maxPitch,  85.0f, "Max Pitch",   0.0f, 89.0f)
    FBZZ_FIELD_RANGE(float, mouseSensitivity, 0.2f, "Mouse Sensitivity", 0.01f, 5.0f)
    FBZZ_TOOLTIP("Option の「マウス感度」が既定値のときの旋回量。設定はこれに掛かる")
    FBZZ_FIELD(bool, mouseOrbit, true, "Mouse Orbit")

    FBZZ_GROUP("Gamepad")
    FBZZ_FIELD(bool, padOrbit, true, "Pad Orbit")
    FBZZ_FIELD_RANGE(float, padLookSpeed, 200.0f, "Pad Look Speed", 10.0f, 720.0f)
    FBZZ_TOOLTIP("Option の「スティック感度」が既定値のときの旋回速度 (度/秒)")
    FBZZ_FIELD(bool, padInvertY, false, "Pad Invert Y")

    FBZZ_GROUP("Ground Smoothing")
    // WHY 縦だけならすか: 段差と坂で CharacterController は接地点へ Y を跳ばす。
    //     そのまま目に乗せると、階段を上るたびに視界が段の高さぶん跳ねる。
    //     横をならさないのは、横の遅れがそのまま「入力が重い」として手に残るから。
    FBZZ_FIELD_RANGE(float, verticalFollowSpeed, 20.0f, "Vertical Follow", 0.0f, 60.0f)
    FBZZ_TOOLTIP("接地中に目の高さが追い付く速さ。0 でならさない")
    FBZZ_FIELD_RANGE(float, snapDistance, 1.5f, "Snap Distance", 0.1f, 20.0f)
    FBZZ_TOOLTIP("この距離以上ずれたらならさず飛ぶ。復活や転送で視界が引きずられない")

    FBZZ_GROUP("View Bob")
    // WHY 時間ではなく歩いた距離で位相を進めるか: 時間で進めると、立ち止まっている間も
    //     位相だけ走り続け、歩き出しの 1 歩目が毎回違う足から始まる。距離で進めれば
    //     速さが変わっても歩幅は変わらず、歩きと走りが自然に別の周期になる。
    FBZZ_FIELD_RANGE(float, bobAmplitude, 0.045f, "Bob Amplitude", 0.0f, 0.3f)
    FBZZ_TOOLTIP("上下の揺れ幅 (m)。0 で頭の揺れを切る")
    FBZZ_FIELD_RANGE(float, bobSway, 0.030f, "Bob Sway", 0.0f, 0.3f)
    FBZZ_TOOLTIP("左右の揺れ幅 (m)。上下の半分の周期で、2 歩で 1 往復する")
    FBZZ_FIELD_RANGE(float, bobDistance, 3.0f, "Bob Distance", 0.5f, 10.0f)
    FBZZ_TOOLTIP("1 往復に進む距離 (m)。上下はその半分ごと (1 歩ごと) に沈む")
    FBZZ_FIELD_RANGE(float, bobResponse, 8.0f, "Bob Response", 0.0f, 40.0f)
    FBZZ_TOOLTIP("歩き出しと止まりで揺れが立ち上がる / 消える速さ")

    // WHY 揺れ (被弾・着地・起爆) を持たないか: 生成と合成は
    //     CameraShakeManagerComponent の担当で、ここは合成済みのオフセットを
    //     最終位置へ足すだけにする。TPS と同じ形なので、視点を替えても
    //     揺れの調整はやり直しにならない。

    // WHY 向きと位置でフェーズを分けるか:
    //   照準・レーザー・上半身のエイム姿勢は「カメラが今どこを向いているか」から引かれる。
    //   向きを LateScript で決めると、それらが読めるのは次のフレームになる。向きは
    //   マウス入力だけで決まり物理を待たないので、Script フェーズで先に確定させる。
    //   位置は物理適用後のプレイヤー座標が要るため LateScript で追う。
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

    // 生ポインタを保持すると、対象が破棄された次のフレームに解放済みメモリを読む。
    // EntityRef は generation まで Scene 側で検証するため、対象消滅を nullptr として扱える。
    EntityRef m_target;

    bool  m_hasEye = false;
    float m_eyeY   = 0.0f;

    bool    m_hasBobOrigin = false;
    Vector3 m_bobOrigin    = Vector3::ZERO;
    float   m_bobPhase     = 0.0f;
    float   m_bobWeight    = 0.0f;

    // 自分が止めたものだけを覚える。全部まとめて戻すと、別の理由で消えていた
    // パーツ (演出中の点滅など) まで一人称をやめた瞬間に現れる。
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

    // 受聴点をカメラへ置く。一人称では耳も目と同じ場所にあり、左右の聞こえ方が
    // 画面の見え方とそのまま一致する。
    //
    // WHY ここで足すか: Listener がシーンに 1 つも無いと AudioSystem は距離減衰の
    //     基準を持てず、3D 音源が丸ごと鳴らない。しかも警告は出ないので、
    //     症状は「敵の音だけ無音」という形でしか現れない。
    se::EnsureListener(scene);
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
    transform.rotation = CurrentRotation();
}

inline void FpsCameraComponent::OnLateUpdate()
{
    if (!transform) return;
    GameObject* target = ResolveTarget();
    if (!target) return;

    ApplyBodyVisibility(*target);

    // 向きは OnUpdate で確定済み。ここで入力を読み直すと 1 フレームに 2 回転する。
    const Quaternion rotation = CurrentRotation();
    // ピッチは右手軸まわりなので、右ベクトルは水平のまま。前は俯角のぶんだけ下を向くため、
    // 目を前へ出すのには水平成分を使う (下を向いた瞬間に目が沈むのを避ける)。
    const Vector3 right   = rotation * Vector3::RIGHT;
    Vector3       forward = rotation * Vector3::FORWARD;
    forward.y = 0.0f;
    const Vector3 planarForward = forward.NormalizedOr(Vector3::ZERO);

    const Vector3 foot = target->transform.worldPosition;
    Vector3 eye = foot + Vector3::UP * eyeHeight + planarForward * eyeForward;

    const float safeDt   = Max(Time::deltaTime, 0.0f);
    const bool  grounded = IsGrounded(*target);

    // 空中は 1:1 で追う。跳躍と落下は縦の変位そのものが手応えなので、ならすと
    // 跳んだ実感が消える。復活や転送の飛びは距離で見分けて素通しする。
    if (m_hasEye && grounded && Abs(eye.y - m_eyeY) < Max(snapDistance, EPSILON))
        eye.y = Lerp(m_eyeY, eye.y, FollowRate(verticalFollowSpeed, safeDt));
    m_eyeY   = eye.y;
    m_hasEye = true;

    eye += TickBob(foot, right, grounded, safeDt);

    // 画角は Option の「視野角」が基準で、起爆の張り出しをそこへ足す。
    // WHY 毎フレーム書くか: 基準そのものが設定変更で動く。1 度だけ書くと、
    //     Option で視野角を変えても遊びに戻るまで反映されない。
    float fovOffset = 0.0f;
    if (auto* follow = CameraFollowManagerComponent::Instance())
        fovOffset = follow->FovOffset();
    if (auto* camera = scene.GetComponent<CameraComponent>())
        camera->fovY = GameSettingsComponent::GameOrDefault().fov + fovOffset;

    // 合成済みの揺れをカメラのローカル軸へ乗せる。生成も減衰もマネージャー側の仕事。
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
    // 歩ける速さを超えていたら移動ではなく転送。位相も揺れも進めない。
    if (speed > 100.0f) {
        distance = 0.0f;
        speed    = 0.0f;
    }

    // 止まりと着地で揺れを切るときは、そのまま振幅だけ落とす。位相を止めると
    // 沈んだ位置で固まって見える。
    const float wanted = (grounded && speed > 0.2f) ? 1.0f : 0.0f;
    m_bobWeight = Lerp(m_bobWeight, wanted, FollowRate(bobResponse, dt));
    if (m_bobWeight < 0.01f) {
        // 揺れが消えたところで位相を畳む。次の歩き出しが必ず同じ足から始まる。
        m_bobPhase  = 0.0f;
        m_bobWeight = 0.0f;
        return Vector3::ZERO;
    }

    m_bobPhase = std::fmod(m_bobPhase + (distance / Max(bobDistance, EPSILON)) * TWO_PI, TWO_PI);
    // 上下は 1 歩ごと、左右は 2 歩で 1 往復。人の歩きは左右の足で重心が入れ替わるので、
    // 横の周期は縦の半分になる。
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
    // 編集中は触らない。エディタでメッシュを消すと、その状態がシーンの中身になる。
    const bool wantHidden = hideBody && app.IsPlaying();
    if (wantHidden == m_bodyHidden) return;
    m_bodyHidden = wantHidden;

    if (wantHidden) HideBody(target);
    else            ShowBody();
}

inline void FpsCameraComponent::HideBody(GameObject& target)
{
    // WHY 直下の子だけを見るか: モデルのパーツは FBX の分割そのままに Player の
    //     直下へ並ぶ。銃はボーンのソケット (もっと深い階層) にぶら下がるので、
    //     部分木を丸ごと辿ると手にした銃まで消える。
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
    // 0.001 は「1/followSpeed 秒でここまで残る」の意味。速さを秒に読み替えられる。
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
    // 対象が入れ替わったら、前の体を隠したままにしない。
    ShowBody();
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
