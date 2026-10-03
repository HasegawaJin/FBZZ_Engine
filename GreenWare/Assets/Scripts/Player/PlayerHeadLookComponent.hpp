/// @file    PlayerHeadLookComponent.hpp
/// @brief   照準の先へ頭だけを向け、狙いの連鎖に「視線」の段を足す
/// @author  Hasegawa Jin
/// @date    2026-08-22

/// @note 頭だけ IK で回す: 胸/腕は 9 セルのポーズ側で配分済みで同じ胴を回すと二重に効くが、
/// @note 頭は配分が薄く (15度 vs リグ限界60度) 余裕がある (配分表は minibot-animation-export.md)。
/// @note 移動中は体が進行方向へ固定されるため、頭だけが狙いを追える唯一の段になる。
/// @note 上流 (胸のひねり・足IK) が頭の姿勢を動かすため、解決は毎フレーム最後に行う。
#pragma once
#include <Scripts/Game/TimeManagerComponent.hpp>

#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Player/PlayerAimComponent.hpp>
#include <Scripts/Player/PlayerControllerComponent.hpp>
#include <Scripts/Player/WeaponRigComponent.hpp>
#include <Scripts/Utils/PlayerActionState.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class PlayerHeadLookComponent : public Script {
    FBZZ_SCRIPT(PlayerHeadLookComponent)

    /// @note 無ければ OnStart で足す。IK が無い状態でも他のモジュールは成立するため必須にしない。
    FBZZ_OPTIONAL_COMPONENT(IKSolverComponent)

public:
    FBZZ_GROUP("Head Look")
    FBZZ_FIELD(std::string, headBoneName, "Head", "Head Bone")
    FBZZ_FIELD_RANGE(float, headWeight, 0.85f, "Head Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 で無効。1 にすると頭のアニメーションを完全に上書きする")
    /// @note リグの LIMIT_ROTATION は Head ヨー ±60 度。これを超える値を入れても
    /// @note Blender 側の拘束と食い違うだけで、絵としては伸びない。
    FBZZ_FIELD_RANGE(float, clampDegrees, 55.0f, "Clamp Degrees", 0.0f, 90.0f)
    FBZZ_TOOLTIP("首を回せる限界。リグの拘束が ±60 度なのでそれ以内に収める")
    /// @note 頭・胸・腕・腰が同じ速さで動くと、ひねりが一枚板に見える。腕より速くして
    /// @note 頭が先に振れる時間差を作ると「人が振り向いた」に読み替えられる。
    FBZZ_FIELD_RANGE(float, response, 18.0f, "追従", 1.0f, 40.0f)
    FBZZ_TOOLTIP("腕 (Aim Response 14) より速くすると、頭が先に振れて体が追う段差が出る")
    FBZZ_FIELD_RANGE(float, weightResponse, 6.0f, "Weight Response", 1.0f, 30.0f)
    FBZZ_TOOLTIP("対象を失ったときに視線が正面へ戻る速さ")
    /// @note TickAimTurn は収納中の旋回を止めている (体だけ追うと不気味)。頭だけなら
    /// @note 「気にしている」に読めるので、収納中かどうかの判断を持ち込まない。
    FBZZ_FIELD(bool, lookWhenHolstered, true, "Look When Holstered")

    FBZZ_GROUP("体ごと向く")
    /// @note 首の拘束は ±60 度 (clampDegrees) で、張り付いたまま回し続けると «頭が限界で
    /// @note 固まった人» になる。限界で体を回して首を中央へ戻すのが TPS の «振り向いた»。
    FBZZ_FIELD(bool, turnBodyBeyondClamp, true, "限界を越えたら体ごと向く")
    FBZZ_FIELD_RANGE(float, bodyTurnDegrees, 50.0f, "体が動き出す角度", 10.0f, 180.0f)
    FBZZ_TOOLTIP("カメラ前方と体の正面がこれだけ開いたら体が回り始める。"
                 "Clamp Degrees より小さくしないと «首が限界で止まってから回る» になる")
    /// @note 入る角と同じ角で切ると、境目でカメラを微動させるたびに体が震える。
    /// @note 止める角は入る角より内側に持つ。
    FBZZ_FIELD_RANGE(float, bodyTurnReleaseDegrees, 12.0f, "止まる角度", 0.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, bodyTurnMaxSpeed, 1.5f, "止まっているとみなす速さ [m/s]", 0.0f, 10.0f)
    FBZZ_TOOLTIP("歩いている間は進む向きが体を回すので、こちらは手を出さない")
    FBZZ_FIELD_READ_ONLY(float, debugHeadYaw, 0.0f, "カメラとのずれ [度]")

    FBZZ_GROUP("軸")
    /// @note 頭のローカル軸のうちどれが前かは FBX の軸変換とボーンロールで決まり、
    /// @note リグを差し替えると変わる。既定は実行時に解決し、狂ったときだけ手で入れる。
    FBZZ_FIELD(bool, autoResolveAxis, true, "Auto Resolve Axis")
    FBZZ_TOOLTIP("頭の姿勢からモデル正面 / 上に最も近いローカル軸を選ぶ")
    FBZZ_FIELD(Vector3, lookAxisOverride, (Vector3::FORWARD), "Look Axis")
    FBZZ_FIELD(Vector3, upAxisOverride, (Vector3::UP), "Up Axis")
    FBZZ_TOOLTIP("ゼロにすると首の横倒しを補正しない")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, drawDebugLine, false, "Draw Debug Line")
    FBZZ_FIELD_READ_ONLY(float, debugWeight, 0.0f, "Applied Weight")
    FBZZ_FIELD_READ_ONLY(std::string, debugAxes, "", "Resolved Axes")

    /// @note PlayerComponent が内部モジュールとして持つときに、同じ PlayerAimComponent を渡す。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    /// @note モデルのヨーオフセットを二重に持たないため、正面の定義は控えるほうから借りる。
    void SetController(PlayerControllerComponent* controller) { m_controller = controller; }
    /// @note 銃を構えているかは銃の側の事実。収納中に見るかどうかの判断に使う。
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

private:
    /// @note 体を回している最中か。入る角と止まる角を別に持つためのラッチ。
    bool m_turningBody = false;
public:

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    static constexpr const char* kTargetName = "HeadLookTarget";
    /// @note 足 IK (0) や背骨 (20) より後。上流が動いた結果の頭へ最後に効かせる。
    static constexpr int kChainOrder = 100;
    /// @note 視線が要らない間も chain.weight をここまでしか下げない。
    /// @note IKSystem は weight <= 0 のチェーンを丸ごと飛ばすが、SolveLookAt が持つ
    /// @note lookAtSmoothedRotation はワールドの絶対回転で、飛ばされている間は古い姿勢の
    /// @note まま凍る。薄く回し続けて FK の頭へ収束させないと、次に掴んだフレームで
    /// @note 古い回転との差分が一度に効き、頭が一瞬あらぬ方向を向く。
    static constexpr float kWeightFloor = 0.002f;
    /// @note 狙点が近すぎると向きが求まらないため、視線の的はこの距離へ置き直す。
    static constexpr float kIdleTargetDistance = 8.0f;

    /// @note ベクトルに最も近い直交軸を単位ベクトルで返す。
    [[nodiscard]] static Vector3 NearestCardinalAxis(const Vector3& v);

    [[nodiscard]] PlayerAimComponent* Aim() const;
    [[nodiscard]] std::string TargetName() const;
    [[nodiscard]] Quaternion ModelRotation() const;
    [[nodiscard]] GameObject* HeadBone();
    /// @note 頭のローカル軸のうち、モデルの正面 / 上に最も近い直交軸を選ぶ。
    bool ResolveAxes(GameObject& headBone);
    /// @note 首の限界を越えたぶんだけ体を回す。回している間は毎フレーム要求し続ける。
    void TurnBodyIfNeeded(const Vector3& cameraForward);
    /// @note 視線の的を用意する。DLL リロードで Script だけ作り直されても増やさない。
    void EnsureTarget();
    /// @note Player は IKSolverComponent を持っていないので、視線を使う側が用意する。
    void EnsureSolver();
    /// @note AimAt チェーンを 1 本だけ確保して返す。見つからなければ足す。
    [[nodiscard]] IKChain* EnsureChain(GameObject& proxy);

    PlayerAimComponent*        m_aimOverride = nullptr;
    PlayerControllerComponent* m_controller  = nullptr;
    WeaponRigComponent*        m_weaponRig   = nullptr;

    EntityRef m_target;
    /// @note 毎フレーム名前で探すと 58 ボーンを走査することになる。
    EntityRef   m_headBone;
    std::string m_cachedBoneName;

    Vector3 m_lookAxis = Vector3::FORWARD;
    Vector3 m_upAxis   = Vector3::UP;
    bool    m_axesResolved = false;
    bool    m_warnedNoHead = false;
    bool    m_warnedNoController = false;

    float m_weight = 0.0f;
};

FBZZ_REFLECT(PlayerHeadLookComponent)

inline Vector3 PlayerHeadLookComponent::NearestCardinalAxis(const Vector3& v)
{
    const float ax = std::abs(v.x);
    const float ay = std::abs(v.y);
    const float az = std::abs(v.z);
    if (ax >= ay && ax >= az) return { v.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f };
    if (ay >= az)             return { 0.0f, v.y >= 0.0f ? 1.0f : -1.0f, 0.0f };
    return { 0.0f, 0.0f, v.z >= 0.0f ? 1.0f : -1.0f };
}

inline PlayerAimComponent* PlayerHeadLookComponent::Aim() const
{
    return m_aimOverride ? m_aimOverride : scene.GetScript<PlayerAimComponent>();
}

inline std::string PlayerHeadLookComponent::TargetName() const
{
    /// @note 的はルートに置くため、同名だと 2 人目のプレイヤーが 1 人目の的を奪う。
    GameObject* owner = scene.Self();
    return std::string(kTargetName) + "_" + (owner ? owner->instanceId : std::string{});
}

inline Quaternion PlayerHeadLookComponent::ModelRotation() const
{
    return m_controller ? m_controller->ModelRotation() : transform.worldRotation;
}

inline GameObject* PlayerHeadLookComponent::HeadBone()
{
    /// @note Inspector で名前を書き換えたら探し直す。キャッシュだけ古い骨を指し続けると、
    /// @note チェーンは新しい骨を回しているのに軸は古い骨から解決した値のままになる。
    if (m_cachedBoneName != headBoneName) {
        m_cachedBoneName = headBoneName;
        m_headBone       = {};
        m_axesResolved   = false;
        m_warnedNoHead   = false;
    }
    if (GameObject* cached = m_headBone.Resolve(scene))
        return cached;

    GameObject* self = scene.Self();
    if (!self || headBoneName.empty())
        return nullptr;

    GameObject* bone = FindInSubtree(*self, headBoneName);
    if (!bone) {
        if (!m_warnedNoHead) {
            m_warnedNoHead = true;
            /// @note 黙って抜けると「視線だけ効かない」という、画面から原因の見えない状態になる。
            debug.LogWarning("PlayerHeadLookComponent: head bone '" + headBoneName
                             + "' not found; head look is disabled.");
        }
        return nullptr;
    }
    m_headBone = EntityRef{ bone->GetID() };
    return bone;
}

inline bool PlayerHeadLookComponent::ResolveAxes(GameObject& headBone)
{
    if (!autoResolveAxis) {
        m_lookAxis = lookAxisOverride;
        m_upAxis   = upAxisOverride;
    } else {
        /// @note コントローラー必須: モデルは 180 度のヨーオフセットを持つ。知らずに
        /// @note transform.worldRotation を正面にすると選ばれる軸が裏返り、頭が敵と
        /// @note 正反対を向いたまま静かに成立する。
        if (!m_controller) {
            if (!m_warnedNoController) {
                m_warnedNoController = true;
                debug.LogError("PlayerHeadLookComponent requires the controller for the model "
                               "yaw offset. Attach it through PlayerComponent, or turn off "
                               "Auto Resolve Axis and set Look Axis by hand.");
            }
            return false;
        }
        const Quaternion worldToBone = headBone.transform.worldRotation.Inverse();
        m_lookAxis = NearestCardinalAxis(worldToBone * (ModelRotation() * Vector3::FORWARD));
        m_upAxis   = NearestCardinalAxis(worldToBone * Vector3::UP);
    }

    if (m_lookAxis.LengthSq() <= EPSILON)
        return false;

    /// @note 前と上が同じ軸に落ちるのは、頭が 45 度以上ピッチした姿勢で解決したとき。
    /// @note そのまま渡すとロール補正が退化するので、補正そのものを切る (エンジン側が
    /// @note lookAtUpAxis のゼロを「補正しない」として扱う)。
    if (std::abs(Vector3::Dot(m_lookAxis, m_upAxis)) > 0.5f)
        m_upAxis = Vector3::ZERO;

    /// @note 手で入れているうちは latch しない。Inspector で軸を触った値がその場で効き、
    /// @note Auto へ戻したときも解決からやり直せる。
    m_axesResolved = autoResolveAxis;
    const auto axisLabel = [](const Vector3& axis) {
        return "(" + std::to_string(static_cast<int>(axis.x)) + ","
                   + std::to_string(static_cast<int>(axis.y)) + ","
                   + std::to_string(static_cast<int>(axis.z)) + ")";
    };
    debugAxes = "look" + axisLabel(m_lookAxis) + " up" + axisLabel(m_upAxis);
    return true;
}

inline void PlayerHeadLookComponent::EnsureTarget()
{
    /// @note スクリプト DLL リロードでこの Script は作り直され EntityRef は空に戻るが、
    /// @note 的の GameObject は Scene 側に残る。先に拾い直さないとリロードのたびに増える。
    if (GameObject* existing = scene.Find(TargetName(), true)) {
        m_target = EntityRef{ existing->GetID() };
        return;
    }

    /// @note 的は「頭が向くべきワールドの一点」でしかなく、プレイヤーの子にしない。
    /// @note 親を持たせると置きたい座標をいちいち親のローカルへ落とす必要が出る。
    GameObject* proxy = scene.Create(TargetName());
    if (!proxy) return;
    proxy->runtimeGenerated = true;
    m_target = EntityRef{ proxy->GetID() };
}

inline void PlayerHeadLookComponent::EnsureSolver()
{
    GameObject* self = scene.Self();
    if (!self)
        return;

    /// @note コンポーネント追加は ECS の格納そのものを動かす。毎フレームの OnUpdate から
    /// @note 行うと他のシステムが巡回中に配列が動きうるため、OnStart で 1 度だけ組み立てる。
    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik)
        ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;
}

inline IKChain* PlayerHeadLookComponent::EnsureChain(GameObject& proxy)
{
    GameObject* self = scene.Self();
    if (!self)
        return nullptr;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik)
        return nullptr;

    /// @note 名前ではなく order で見分ける。Inspector で Head Bone を書き換えた瞬間に
    /// @note 名前で探すと一致しなくなり、古いチェーンを残したまま 2 本目を足してしまう。
    for (IKChain& chain : ik->chains) {
        if (chain.type == IKSolverType::AimAt && chain.order == kChainOrder) {
            /// @note 的はランタイム生成なのでシーンには保存されない。参照は毎回張り直す。
            chain.targetEntity = proxy.GetID();
            return &chain;
        }
    }

    IKChain chain{};
    chain.type         = IKSolverType::AimAt;
    chain.boneNames    = { headBoneName };
    chain.order        = kChainOrder;
    chain.enabled      = true;
    chain.weight       = kWeightFloor;
    chain.targetEntity = proxy.GetID();
    ik->chains.push_back(std::move(chain));
    return &ik->chains.back();
}

inline void PlayerHeadLookComponent::OnStart()
{
    m_headBone           = {};
    m_cachedBoneName     = headBoneName;
    m_axesResolved       = false;
    m_warnedNoHead       = false;
    m_warnedNoController = false;
    m_weight             = 0.0f;
    debugWeight          = 0.0f;
    debugAxes.clear();

    EnsureTarget();
    EnsureSolver();
}

inline void PlayerHeadLookComponent::OnUpdate()
{
    GameObject* proxy = m_target.Resolve(scene);
    if (!proxy)
        return;

    GameObject* head = HeadBone();
    if (!head)
        return;

    /// @note 自動解決は最初の 1 フレームだけ。OnStart の時点ではまだ Animator が
    /// @note ポーズを作っておらず、頭のワールド回転がバインドポーズのままになりうる。
    if (!m_axesResolved && !ResolveAxes(*head))
        return;

    IKChain* chain = EnsureChain(*proxy);
    if (!chain)
        return;

    /// @note Head Weight 0 は「首を触らない」という指示。薄く回し続ける必要も無い。
    /// @note lookAtHasState を落として、次に戻したときに FK の頭から始め直させる。
    if (headWeight <= 0.0f) {
        chain->enabled        = false;
        chain->lookAtHasState = false;
        m_weight    = 0.0f;
        debugWeight = 0.0f;
        return;
    }

    /// @note ロックオン廃止後は頭が向くべき先が「選ばれた 1 体」でなく「今どこへ線を
    /// @note 引こうとしているか」になる。頭はカメラ前方 (プレイヤーが見ている方) を向く。
    /// @note 敵の方を向かせると、画面外の相手へ首が張り付いてあらぬ方向を向いたまま走る。
    const bool drawn = !m_weaponRig || m_weaponRig->IsDrawn();
    Vector3 cameraForward = Vector3::ZERO;
    if (GameObject* camera = scene.GetMainCameraObject())
        cameraForward = camera->transform.forward.NormalizedOr(Vector3::ZERO);
    const bool wants = cameraForward.LengthSq() > EPSILON && (drawn || lookWhenHolstered);
    const Vector3 headPosition = head->transform.worldPosition;

    /// @note kWeightFloor で薄く回し続けるため、見ない間も的を正面へ置き直す。
    /// @note 置き去りにすると残りかすが古い方向へ効き続ける。
    const Vector3 lookPoint = headPosition
        + (wants ? cameraForward : (ModelRotation() * Vector3::FORWARD))
              * kIdleTargetDistance;
    proxy->transform.position      = lookPoint;
    proxy->transform.worldPosition = lookPoint;

    /// @note 有効/無効ではなくウェイトを補間する。銃を抜く/収める・カメラが向きを失う、
    /// @note いずれも wants は二値で切り替わるため、そのまま扱うと首が跳ねる。
    const float desired = wants ? Clamp01(headWeight) : 0.0f;
    const float res = 1.0f - std::exp(
        -std::max(weightResponse, 0.0f) * std::max(TimeManagerComponent::PlayerDeltaTime(), 0.0f));
    m_weight += (desired - m_weight) * res;

    /// @note Inspector で触った値がその場のフレームから効くよう、毎回入れ直す。
    /// @note ボーン名だけは毎フレーム組み直すと vector を作り直すことになるため、変化時のみ。
    if (chain->boneNames.size() != 1 || chain->boneNames[0] != headBoneName)
        chain->boneNames = { headBoneName };
    chain->enabled          = true;
    chain->weight           = std::max(m_weight, kWeightFloor);
    chain->lookAtClampAngle = clampDegrees;
    chain->lookAtSpeed      = response;
    chain->lookAtAxis       = m_lookAxis;
    chain->lookAtUpAxis     = m_upAxis;
    debugWeight             = m_weight;

    TurnBodyIfNeeded(cameraForward);

    if (drawDebugLine && wants)
        debug.DrawLine(headPosition, lookPoint, { 0.4f, 0.8f, 1.0f, 1.0f });
}

/// @note 首が限界 (clampDegrees) に張り付く前に体を回し、視線を首の可動域の内側へ戻す。
/// @note 移動中は進む向きが体を回しているため手を出さない (両方が RequestFacing を
/// @note 出すと要求が毎フレーム入れ替わり体が震える)。斬撃と弾きが «斬る向き» で
/// @note RequestFacing を出している間も手を出さない (上書きすると刃の向きがずれる。
/// @note このモジュールは m_controller より後に回るため黙って勝ってしまう)。
inline void PlayerHeadLookComponent::TurnBodyIfNeeded(const Vector3& cameraForward)
{
    debugHeadYaw = 0.0f;
    if (!turnBodyBeyondClamp || !m_controller) { m_turningBody = false; return; }

    Vector3 wanted = cameraForward;
    wanted.y = 0.0f;
    if (wanted.LengthSq() <= EPSILON) { m_turningBody = false; return; }
    wanted = wanted.Normalized();

    Vector3 facing = ModelRotation() * Vector3::FORWARD;
    facing.y = 0.0f;
    if (facing.LengthSq() <= EPSILON) { m_turningBody = false; return; }
    facing = facing.Normalized();

    const float dot   = std::clamp(Vector3::Dot(facing, wanted), -1.0f, 1.0f);
    const float yaw   = ToDeg(std::acos(dot));
    debugHeadYaw = yaw;

    const auto blade = playeraction::Read(Time::time);
    if (blade.swinging || blade.recovering) { m_turningBody = false; return; }
    if (animator.GetFloat("Speed") > std::max(bodyTurnMaxSpeed, 0.0f)) {
        m_turningBody = false;
        return;
    }

    if (!m_turningBody && yaw >= std::max(bodyTurnDegrees, 1.0f)) m_turningBody = true;
    if (m_turningBody && yaw <= std::max(bodyTurnReleaseDegrees, 0.0f)) m_turningBody = false;
    if (m_turningBody) m_controller->RequestFacing(wanted);
}
inline void PlayerHeadLookComponent::OnDestroy()
{
    /// @note ルートに置いた以上、プレイヤーと一緒には消えない。持ち主が畳む。
    if (GameObject* proxy = m_target.Resolve(scene))
        scene.Destroy(*proxy);
    m_target = {};
}

} /// @note namespace sandbox
