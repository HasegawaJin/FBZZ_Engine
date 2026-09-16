/// @file    PlayerHeadLookComponent.hpp
/// @brief   照準の先へ頭だけを向け、狙いの連鎖に「視線」の段を足す
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 胸ではなく頭だけを IK で回すか:
///   ヨー ±45 度のエイムコーンは 9 セルのポーズ側で Chest 30 度 / UpperArm 15 度に
///   配分済みで、胴体は既にひねられている (minibot-animation-export.md 6 節の配分表)。
///   同じ胸を IK でもう一度回すと二重に効き、腕が相手を撃ち越す。加えて同ドキュメント
///   9 節は「AimAt IK によるエイム」を、絵作りを制御できないという理由で廃止済みとしている。
///   一方 Head へのヨー配分はポーズ側で 15 度しかなく、リグの LIMIT_ROTATION は ±60 度ある。
///   頭は銃を持たないので廃止の理由に当たらず、余っている 45 度をそのまま視線に使える。
///
/// WHY 走っている間にこそ効くか:
///   移動中は PlayerControllerComponent が体を進行方向へ向け、TickAimTurn も走らない。
///   狙いのヨーを吸収できるのはコーンの ±45 度だけになり、それを超えた相手に体は
///   何もできない。頭は体の向きから独立して回る唯一の段なので、
///   「あいつを見ながら走っている」を成立させられるのはここだけになる。
///
/// WHY 頭を最後に解くか:
///   ポーズ側の Chest ひねりも、足 IK による腰の上下も、頭のワールド姿勢を動かす。
///   上流が動いた結果に対して向きを取り直さないと、狙点から少しずつずれる。
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

    // 無ければ OnStart で足す。IK が無い状態でも他のモジュールは成立するため必須にしない。
    FBZZ_OPTIONAL_COMPONENT(IKSolverComponent)

public:
    FBZZ_GROUP("Head Look")
    FBZZ_FIELD(std::string, headBoneName, "Head", "Head Bone")
    FBZZ_FIELD_RANGE(float, headWeight, 0.85f, "Head Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 で無効。1 にすると頭のアニメーションを完全に上書きする")
    // リグの LIMIT_ROTATION は Head ヨー ±60 度。これを超える値を入れても
    // Blender 側の拘束と食い違うだけで、絵としては伸びない。
    FBZZ_FIELD_RANGE(float, clampDegrees, 55.0f, "Clamp Degrees", 0.0f, 90.0f)
    FBZZ_TOOLTIP("首を回せる限界。リグの拘束が ±60 度なのでそれ以内に収める")
    // WHY 腕より速くするか: 頭・胸・腕・腰が同じ速さで動くと、ひねりが一枚板に見える。
    //     頭が先に振れて胴体が追う時間差が「人が振り向いた」に読み替えられる。
    FBZZ_FIELD_RANGE(float, response, 18.0f, "追従", 1.0f, 40.0f)
    FBZZ_TOOLTIP("腕 (Aim Response 14) より速くすると、頭が先に振れて体が追う段差が出る")
    FBZZ_FIELD_RANGE(float, weightResponse, 6.0f, "Weight Response", 1.0f, 30.0f)
    FBZZ_TOOLTIP("対象を失ったときに視線が正面へ戻る速さ")
    // WHY 収納中も見るか: TickAimTurn は収納中の旋回を止めている (体だけ追うと不気味)。
    //     頭だけなら「気にしている」に読めるので、同じ判断を持ち込まない。
    FBZZ_FIELD(bool, lookWhenHolstered, true, "Look When Holstered")

    FBZZ_GROUP("体ごと向く")
    // WHY 頭だけで足りないか: 首の拘束は ±60 度 (clampDegrees) で、そこへ張り付いたまま
    //     カメラを回し続けると «頭が限界で固まった人» になる。限界に達したら体を回して
    //     首を中央へ戻すのが、TPS で «振り向いた» と読める唯一の形。
    FBZZ_FIELD(bool, turnBodyBeyondClamp, true, "限界を越えたら体ごと向く")
    FBZZ_FIELD_RANGE(float, bodyTurnDegrees, 50.0f, "体が動き出す角度", 10.0f, 180.0f)
    FBZZ_TOOLTIP("カメラ前方と体の正面がこれだけ開いたら体が回り始める。"
                 "Clamp Degrees より小さくしないと «首が限界で止まってから回る» になる")
    // WHY 止める角を別に持つか: 同じ角で入り切りすると、境目でカメラを微動させるたびに
    //     体が回ったり止まったりして震える。入る角より内側で止める。
    FBZZ_FIELD_RANGE(float, bodyTurnReleaseDegrees, 12.0f, "止まる角度", 0.0f, 90.0f)
    FBZZ_FIELD_RANGE(float, bodyTurnMaxSpeed, 1.5f, "止まっているとみなす速さ [m/s]", 0.0f, 10.0f)
    FBZZ_TOOLTIP("歩いている間は進む向きが体を回すので、こちらは手を出さない")
    FBZZ_FIELD_READ_ONLY(float, debugHeadYaw, 0.0f, "カメラとのずれ [度]")

    FBZZ_GROUP("軸")
    // 頭のローカル軸のうちどれが前かは FBX の軸変換とボーンロールで決まり、
    // リグを差し替えると変わる。既定は実行時に解決し、狂ったときだけ手で入れる。
    FBZZ_FIELD(bool, autoResolveAxis, true, "Auto Resolve Axis")
    FBZZ_TOOLTIP("頭の姿勢からモデル正面 / 上に最も近いローカル軸を選ぶ")
    FBZZ_FIELD(Vector3, lookAxisOverride, (Vector3::FORWARD), "Look Axis")
    FBZZ_FIELD(Vector3, upAxisOverride, (Vector3::UP), "Up Axis")
    FBZZ_TOOLTIP("ゼロにすると首の横倒しを補正しない")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, drawDebugLine, false, "Draw Debug Line")
    FBZZ_FIELD_READ_ONLY(float, debugWeight, 0.0f, "Applied Weight")
    FBZZ_FIELD_READ_ONLY(std::string, debugAxes, "", "Resolved Axes")

    /// PlayerComponent が内部モジュールとして持つときに、同じ PlayerAimComponent を渡す。
    void SetAimComponent(PlayerAimComponent* aim) { m_aimOverride = aim; }
    /// モデルのヨーオフセットを二重に持たないため、正面の定義は控えるほうから借りる。
    void SetController(PlayerControllerComponent* controller) { m_controller = controller; }
    /// 銃を構えているかは銃の側の事実。収納中に見るかどうかの判断に使う。
    void SetWeaponRig(WeaponRigComponent* rig) { m_weaponRig = rig; }

private:
    /// 体を回している最中か。入る角と止まる角を別に持つためのラッチ。
    bool m_turningBody = false;
public:

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    static constexpr const char* kTargetName = "HeadLookTarget";
    /// 足 IK (0) や背骨 (20) より後。上流が動いた結果の頭へ最後に効かせる。
    static constexpr int kChainOrder = 100;
    /// 視線が要らない間も chain.weight をここまでしか下げない。
    /// WHY: IKSystem は weight <= 0 のチェーンを丸ごと飛ばすが、SolveLookAt が持つ
    ///      lookAtSmoothedRotation は「ワールドの絶対回転」で、飛ばされている間は
    ///      古い姿勢のまま凍る。次に対象を掴んだフレームで古い回転との差分が一度に
    ///      適用され、頭が一瞬あらぬ方向を向く。薄く回し続ければ、平滑値は FK の頭へ
    ///      収束したまま待機する。
    static constexpr float kWeightFloor = 0.002f;
    /// 狙点が近すぎると向きが求まらないため、視線の的はこの距離へ置き直す。
    static constexpr float kIdleTargetDistance = 8.0f;

    /// ベクトルに最も近い直交軸を単位ベクトルで返す。
    [[nodiscard]] static Vector3 NearestCardinalAxis(const Vector3& v);

    [[nodiscard]] PlayerAimComponent* Aim() const;
    [[nodiscard]] std::string TargetName() const;
    [[nodiscard]] Quaternion ModelRotation() const;
    [[nodiscard]] GameObject* HeadBone();
    /// 頭のローカル軸のうち、モデルの正面 / 上に最も近い直交軸を選ぶ。
    bool ResolveAxes(GameObject& headBone);
    /// 首の限界を越えたぶんだけ体を回す。回している間は毎フレーム要求し続ける。
    void TurnBodyIfNeeded(const Vector3& cameraForward);
    /// 視線の的を用意する。DLL リロードで Script だけ作り直されても増やさない。
    void EnsureTarget();
    /// Player は IKSolverComponent を持っていないので、視線を使う側が用意する。
    void EnsureSolver();
    /// AimAt チェーンを 1 本だけ確保して返す。見つからなければ足す。
    [[nodiscard]] IKChain* EnsureChain(GameObject& proxy);

    PlayerAimComponent*        m_aimOverride = nullptr;
    PlayerControllerComponent* m_controller  = nullptr;
    WeaponRigComponent*        m_weaponRig   = nullptr;

    EntityRef m_target;
    // 毎フレーム名前で探すと 58 ボーンを走査することになる。
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

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

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
    // 的はルートに置くため、同名だと 2 人目のプレイヤーが 1 人目の的を奪う。
    GameObject* owner = scene.Self();
    return std::string(kTargetName) + "_" + (owner ? owner->instanceId : std::string{});
}

inline Quaternion PlayerHeadLookComponent::ModelRotation() const
{
    return m_controller ? m_controller->ModelRotation() : transform.worldRotation;
}

inline GameObject* PlayerHeadLookComponent::HeadBone()
{
    // Inspector で名前を書き換えたら探し直す。キャッシュだけ古い骨を指し続けると、
    // チェーンは新しい骨を回しているのに軸は古い骨から解決した値のままになる。
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
            // 黙って抜けると「視線だけ効かない」という、画面から原因の見えない状態になる。
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
        // WHY コントローラーを必須にするか: モデルは 180 度のヨーオフセットを持つ。
        //     それを知らずに transform.worldRotation を正面とすると、選ばれる軸が
        //     ちょうど裏返り、頭が敵と正反対を向いたまま静かに成立してしまう。
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

    // 前と上が同じ軸に落ちるのは、頭が 45 度以上ピッチした姿勢で解決したとき。
    // そのまま渡すとロール補正が退化するので、補正そのものを切る (エンジン側が
    // lookAtUpAxis のゼロを「補正しない」として扱う)。
    if (std::abs(Vector3::Dot(m_lookAxis, m_upAxis)) > 0.5f)
        m_upAxis = Vector3::ZERO;

    // 手で入れているうちは latch しない。Inspector で軸を触った値がその場で効き、
    // Auto へ戻したときも解決からやり直せる。
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
    // WHY 先に拾い直すか: スクリプト DLL をリロードするとこの Script は作り直され、
    //     EntityRef は空に戻る。一方 的の GameObject は Scene 側に残っているので、
    //     無条件に作るとリロードのたびに 1 つずつ増えていく。
    if (GameObject* existing = scene.Find(TargetName())) {
        m_target = EntityRef{ existing->GetID() };
        return;
    }

    // WHY プレイヤーの子にしないか: 的は「頭が向くべきワールドの一点」でしかない。
    //     親を持たせると、置きたい座標をいちいち親のローカルへ落とす必要が出る。
    GameObject& proxy = scene.Create(TargetName());
    proxy.runtimeGenerated = true;
    m_target = EntityRef{ proxy.GetID() };
}

inline void PlayerHeadLookComponent::EnsureSolver()
{
    GameObject* self = scene.Self();
    if (!self)
        return;

    // WHY OnStart で足すか: コンポーネントの追加は ECS の格納そのものを動かす。
    //     毎フレーム走る OnUpdate から行うと、他のシステムが巡回している最中に
    //     配列が動く可能性が残る。組み立ては開始時に 1 度で済ませる。
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

    // WHY ボーン名ではなく order で見分けるか: Inspector で Head Bone を書き換えた
    //     瞬間に名前で探すと一致しなくなり、古いチェーンを残したまま 2 本目を足す。
    //     このチェーンは「視線の段」という役割で 1 本しか要らない。
    for (IKChain& chain : ik->chains) {
        if (chain.type == IKSolverType::AimAt && chain.order == kChainOrder) {
            // 的はランタイム生成なのでシーンには保存されない。参照は毎回張り直す。
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

    // 自動解決は最初の 1 フレームだけ。OnStart の時点ではまだ Animator が
    // ポーズを作っておらず、頭のワールド回転がバインドポーズのままになりうる。
    if (!m_axesResolved && !ResolveAxes(*head))
        return;

    IKChain* chain = EnsureChain(*proxy);
    if (!chain)
        return;

    // Head Weight 0 は「首を触らない」という指示。薄く回し続ける必要も無い。
    // lookAtHasState を落として、次に戻したときに FK の頭から始め直させる。
    if (headWeight <= 0.0f) {
        chain->enabled        = false;
        chain->lookAtHasState = false;
        m_weight    = 0.0f;
        debugWeight = 0.0f;
        return;
    }

    // WHY 敵ではなく照準の先を見るか: 6.4 でロックオンを廃したので、頭が向くべきなのは
    //     「選ばれた 1 体」ではなく「今どこへ線を引こうとしているか」になる。敵が居ない
    //     方向へ照準を振っている間も、視線が先に動く方がなぞりの意図が絵に出る。
    // WHY 敵ではなくカメラの前方を見るか: TPS で頭が向くべきなのは «プレイヤーが
    //     見ている方» で、盤面に居る敵ではない。敵を見させると、画面の外の相手へ
    //     首が張り付いて «あらぬ方向を向いたまま走る» になる。
    const bool drawn = !m_weaponRig || m_weaponRig->IsDrawn();
    Vector3 cameraForward = Vector3::ZERO;
    if (GameObject* camera = scene.GetMainCameraObject())
        cameraForward = camera->transform.forward.NormalizedOr(Vector3::ZERO);
    const bool wants = cameraForward.LengthSq() > EPSILON && (drawn || lookWhenHolstered);
    const Vector3 headPosition = head->transform.worldPosition;

    // WHY 見ない間も的を置き直すか: kWeightFloor で薄く回し続けるため、的を
    //     置き去りにすると残りかすが古い方向へ効き続ける。正面へ置けば、
    //     ウェイトが落ちる過程がそのまま「正面へ戻る」になる。
    const Vector3 lookPoint = headPosition
        + (wants ? cameraForward : (ModelRotation() * Vector3::FORWARD))
              * kIdleTargetDistance;
    proxy->transform.position      = lookPoint;
    proxy->transform.worldPosition = lookPoint;

    // WHY チェーンの有効 / 無効ではなくウェイトを補間するか: 銃を抜く / 収める、
    //     カメラが向きを失う、のいずれでも wants は二値で切り替わる。そのまま扱うと
    //     頭が 1 フレームで正面へ戻り、切り替わるたびに首が跳ねる。
    const float desired = wants ? Clamp01(headWeight) : 0.0f;
    const float res = 1.0f - std::exp(
        -std::max(weightResponse, 0.0f) * std::max(TimeManagerComponent::PlayerDeltaTime(), 0.0f));
    m_weight += (desired - m_weight) * res;

    // Inspector で触った値がその場のフレームから効くよう、毎回入れ直す。
    // ボーン名だけは毎フレーム組み直すと vector を作り直すことになるため、変化時のみ。
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

// 首が限界 (clampDegrees) に張り付く前に体を回し、視線を首の可動域の内側へ戻す。
//
// WHY 歩いている間は手を出さないか: 移動中は進む向きが体を回している。両方が
//   RequestFacing を出すと 1 フレームごとに要求が入れ替わり、体が細かく振れる。
// WHY 振っている間は手を出さないか: 斬撃と弾きは «斬る向き» を自分で決めて
//   RequestFacing を出す。こちらが後から上書きすると、狙った先と刃の向きがずれる。
//   (このモジュールは m_controller より後に回るので、黙って勝ってしまう)
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
    // ルートに置いた以上、プレイヤーと一緒には消えない。持ち主が畳む。
    if (GameObject* proxy = m_target.Resolve(scene))
        scene.Destroy(*proxy);
    m_target = {};
}

} // namespace sandbox
