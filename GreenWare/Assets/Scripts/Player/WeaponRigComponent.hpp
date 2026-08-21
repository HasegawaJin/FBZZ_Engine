// FBZZ Engine
// WeaponRigComponent.hpp | sandbox
// 銃 2 丁の「今どこに付いているか」だけを持つ。抜く / 収める の実体。
//
// WHY 入力から切り離すか:
//   以前は PlayerControllerComponent が「G キーを読む」「上半身クリップを流す」
//   「銃の親を差し替える」を 1 つの関数でやっていた。キーバインドを変えたいだけでも
//   銃の付け替えコードを読む羽目になり、逆に付け替えのタイミングを直すと入力の
//   受け付け条件まで巻き込む。入力は「抜きたい」という意思の入口でしかないので、
//   PlayerControllerComponent は RequestDraw() / RequestHolster() を呼ぶだけにし、
//   実際に何が起きるかは全部こちらへ閉じる。
//
// WHY 上半身クリップの再生もここが持つか:
//   「銃が手へ移る時刻」と「腕が銃を掴むポーズになる時刻」は同じ 1 本の時間軸に
//   乗っていないと合わない。再生の開始をコントローラー側に置くと、時計が 2 つに
//   分かれて必ずズレる。開始と経過時間を同じ場所で持つ。
//
// WHY アニメーションイベントを使わないか:
//   .anim / FBX に打ったイベントは、クリップを差し替えた瞬間に黙って消える。
//   実際 Draw_Pistols.fbx / Holster_Pistols.fbx には WeaponAttach / WeaponHolster が
//   1 つも入っておらず、旧実装は常に「正規化時間 0.60 を超えたら」という保険の側だけで
//   動いていた。イベントの有無で挙動が変わる作りは、壊れても気付けない。
//   ここでは Inspector の秒数だけを唯一の入力とし、クリップの尺にも中身にも依存しない。
//
// WHY 親子を差し替えないか:
//   付け替えは瞬間移動になる。ホルスターから手までは 20cm 以上あるので、瞬間移動は
//   はっきり見える。エンジンの SocketAttachmentComponent は「行き先ソケット名を
//   書き換えるだけで、旧ソケットから新ソケットへ blendDuration 秒かけて移る」ので、
//   このスクリプトは行き先を宣言するだけでよい。移動そのものはエンジンの仕事。
//
// WHY 追従の宣言をシーン / Prefab 側に置くか:
//   以前はこのスクリプトが Play 開始時に SocketAttachmentComponent を生成し、シーンへは
//   保存しない方針だった。スクリプトは編集中に走らないので、その間だけ銃は「ホルスター
//   ボーンの子に置かれた素の Transform」になる。銃の原点とグリップは 90 度ずれている
//   (Blender の Z-up→Y-up 変換がモデルのルート直下に残るため) ので、編集中の見た目と
//   再生中の見た目が食い違い、どちらが正なのか画面から判断できなかった。
//   宣言をシーンに保存すれば ConstraintSystem は編集中も走る (Editor は Play 判定なしで
//   LateUpdate を回している) ため、編集時と再生時が同じ 1 本の計算を通る。
//   このスクリプトの役割は「行き先を切り替えること」だけに縮む。
#pragma once

#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Player/WeaponAnimatorComponent.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class WeaponRigComponent : public Script {
    FBZZ_SCRIPT(WeaponRigComponent)

    // Animator が無ければ上半身クリップが空振りするだけで、付け替えそのものは成立する。
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent)

public:
    FBZZ_GROUP("Weapons")
    // 未設定なら WPN_Pistol_L / WPN_Pistol_R を名前で拾う。
    FBZZ_REF(GameObject, weaponLeft,  "Left Pistol")
    FBZZ_REF(GameObject, weaponRight, "Right Pistol")
    // 銃側の合わせ点。ここが手のソケットに重なるように追従する。
    // 銃の FBX にまだ無い場合は銃の原点が追従する (エンジン側で自動フォールバック)。
    FBZZ_FIELD(std::string, gripSocketName, "SOCKET_Grip", "Grip Socket")
    FBZZ_TOOLTIP("銃の FBX 内のソケット名。空なら銃の原点を手のソケットに合わせる")

    FBZZ_GROUP("Upper Body Animation")
    FBZZ_FIELD(std::string, weaponLayerName,  "UpperBody",      "Weapon Layer")
    FBZZ_FIELD(std::string, drawStateName,    "DrawPistols",    "Draw State")
    FBZZ_FIELD(std::string, holsterStateName, "HolsterPistols", "Holster State")

    FBZZ_GROUP("Timing (秒 / クリップに依存しない)")
    // 抜き始めてから銃が手へ移るまで。腕が腰へ届いてグリップを掴む時刻に合わせる。
    FBZZ_FIELD_RANGE(float, drawTransferDelay,    0.30f, "Draw Transfer Delay",    0.0f, 2.0f)
    // 収め始めてから銃が腰へ戻るまで。
    FBZZ_FIELD_RANGE(float, holsterTransferDelay, 0.45f, "Holster Transfer Delay", 0.0f, 2.0f)
    // 移動そのものにかける時間。エンジンの SocketAttachment がこの秒数で補間する。
    FBZZ_FIELD_RANGE(float, transferBlendDuration, 0.16f, "Transfer Blend", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 にすると瞬間移動になる")
    // 動作全体の長さ。次の入力を受け付けるまでの締め切りでもある。
    FBZZ_FIELD_RANGE(float, drawDuration,    0.90f, "Draw Duration",    0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, holsterDuration, 0.90f, "Holster Duration", 0.05f, 3.0f)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD(bool, drawSocketGizmos, false, "Draw Socket Gizmos")

    // 銃を構えているか。エイムレイヤーの重みと発砲可否がこれを見る。
    // WHY 「動作が終わったか」ではなく「移り終わったか」で立てるか:
    //     銃が手に無いのにエイム差分が乗ると、空の手で狙う絵になる。
    [[nodiscard]] bool IsDrawn() const { return m_drawn; }
    // 抜き / 収めの最中。入力の二度押しと、発砲の抑止に使う。
    [[nodiscard]] bool IsBusy()  const { return m_action != Action::None; }

    // PlayerControllerComponent が入力を受けて呼ぶ。ここから先は入力を知らない。
    void RequestDraw()    { StartAction(true); }
    void RequestHolster() { StartAction(false); }
    // 抜いているなら収める / 収めているなら抜く。1 キー運用へ切り替えるとき用。
    void RequestToggle()  { StartAction(!m_drawn); }

    void OnStart() override;
    void OnUpdate() override;
    void OnDrawGizmos() override;

private:
    enum class Action { None, Draw, Holster };

    void StartAction(bool draw);
    // 銃 1 丁ぶんの追従設定。Play 開始時に 1 度だけ組む。
    void SetupAttachment(HandSide hand);
    // 行き先ソケット名を書き換えるだけ。移動はエンジンが行う。
    void SetWeaponSocket(HandSide hand, bool toHand);
    // 銃側の変形 (コンパクト形態 ⇄ 銃形態) を左右まとめて走らせる。
    void PlayWeaponTransform(bool deploy);

    [[nodiscard]] GameObject* ResolveWeapon(HandSide hand) const;
    [[nodiscard]] SocketAttachmentComponent* AttachmentOf(HandSide hand) const;

    Action m_action     = Action::None;
    float  m_actionTime = 0.0f;
    // 今回の動作で、もう行き先を切り替えたか。
    bool   m_transferred = false;
    bool   m_drawn       = false;
    bool   m_warnedNoWeapon = false;
};

FBZZ_REFLECT(WeaponRigComponent)

inline void WeaponRigComponent::OnStart()
{
    m_action      = Action::None;
    m_actionTime  = 0.0f;
    m_transferred = false;
    m_drawn       = false;

    SetupAttachment(HandSide::Left);
    SetupAttachment(HandSide::Right);

    // 開始状態は収納。appliedSocketName がまだ空なので、エンジン側は補間せず
    // 1 フレーム目からホルスターへスナップする。
    SetWeaponSocket(HandSide::Left,  false);
    SetWeaponSocket(HandSide::Right, false);

    // 銃形態のまま腰に付いていると腕を突き抜けるため、畳んだ形から始める。
    PlayWeaponTransform(false);
}

inline GameObject* WeaponRigComponent::ResolveWeapon(HandSide hand) const
{
    const Ref<GameObject>& reference = hand == HandSide::Right ? weaponRight : weaponLeft;
    if (GameObject* object = reference.Get())
        return object;
    return scene.Find(WeaponObjectName(hand));
}

inline SocketAttachmentComponent* WeaponRigComponent::AttachmentOf(HandSide hand) const
{
    GameObject* weapon = ResolveWeapon(hand);
    return weapon ? weapon->GetComponent<SocketAttachmentComponent>() : nullptr;
}

inline void WeaponRigComponent::SetupAttachment(HandSide hand)
{
    GameObject* weapon = ResolveWeapon(hand);
    if (!weapon) {
        if (!m_warnedNoWeapon) {
            // 黙って抜けると「G を押しても何も起きない」だけになり、原因が見えない。
            debug.LogError("WeaponRigComponent: pistol GameObject not found "
                           "(assign Left/Right Pistol, or name them WPN_Pistol_L / WPN_Pistol_R).");
            m_warnedNoWeapon = true;
        }
        return;
    }

    // シーン / Prefab に宣言が保存されていればそれを使う。無ければ実行時に足す
    //  (旧シーンをそのまま開いても動くようにするための保険)。
    auto* attachment = weapon->GetComponent<SocketAttachmentComponent>();
    if (!attachment)
        attachment = &weapon->AddComponent<SocketAttachmentComponent>();

    attachment->enabled         = true;
    // WHY target を代入しないか:
    //   target を入れると「target の部分木から探す」経路になり、target を持てない編集中
    //   (= 祖先をたどる経路) と解決の仕方が変わる。同じ配置を 2 通りの探索で出すと、
    //   片方だけ壊れたときに画面から切り分けられない。空のままにして両者を 1 本に揃える。
    attachment->localSocketName = gripSocketName;
    attachment->blendDuration   = transferBlendDuration;
    attachment->followPosition  = true;
    attachment->followRotation  = true;
    // 銃のスケールはモデルが持つ値が正。手のボーンのスケールを持ち込まない。
    attachment->followScale     = false;
}

inline void WeaponRigComponent::SetWeaponSocket(HandSide hand, bool toHand)
{
    SocketAttachmentComponent* attachment = AttachmentOf(hand);
    if (!attachment)
        return;
    // Inspector で秒数を触った直後も次の切り替えから効くようにする。
    attachment->blendDuration = transferBlendDuration;
    attachment->socketName = toHand ? HandSocketName(hand) : HolsterSocketName(hand);
}

inline void WeaponRigComponent::PlayWeaponTransform(bool deploy)
{
    const HandSide hands[2] = { HandSide::Left, HandSide::Right };
    for (const HandSide hand : hands) {
        GameObject* weapon = ResolveWeapon(hand);
        if (!weapon) continue;
        auto* animation = scene.GetScript<WeaponAnimatorComponent>(weapon);
        if (!animation) continue;
        if (deploy) animation->PlayDeploy();
        else        animation->PlayFold();
    }
}

inline void WeaponRigComponent::StartAction(bool draw)
{
    // 進行中は割り込ませない。抜きかけで収め始めると、上半身クリップと
    // 追従先が別々の向きへ走り出し、どちらが正か分からない状態になる。
    if (m_action != Action::None)
        return;
    if (draw == m_drawn)
        return;

    m_action      = draw ? Action::Draw : Action::Holster;
    m_actionTime  = 0.0f;
    m_transferred = false;

    // WHY SetTrigger を使わないか:
    //   Animator の AnyState 遷移は「今と同じステートへの遷移」をスキップする
    //   (AnimatorSystem.cpp の TryStartTransitionScoped)。スキップされた遷移は
    //   ConsumeTriggers を通らないので、PlayLayerState で先にステートを合わせてから
    //   Trigger も立てると、その Trigger は消費されずに居残る。居残った Trigger は
    //   クリップが終わって別ステートへ抜けた次のフレームに発火し、抜く動作がもう一度
    //   頭から再生される。ステートを直接指定する以上、Trigger は立てない。
    animator.PlayLayerState(weaponLayerName, draw ? drawStateName : holsterStateName);

    // 変形は入力と同時に始める。Deploy は 20F あり、銃が手に移る時刻より長いので、
    // 先行させて初めて「取り出しながら展開する」に見える。
    PlayWeaponTransform(draw);
}

inline void WeaponRigComponent::OnUpdate()
{
    if (m_action == Action::None)
        return;

    m_actionTime += std::max(Time::deltaTime, 0.0f);
    const bool draw = m_action == Action::Draw;

    const float transferAt = std::max(draw ? drawTransferDelay : holsterTransferDelay, 0.0f);
    if (!m_transferred && m_actionTime >= transferAt) {
        m_transferred = true;
        m_drawn       = draw;
        SetWeaponSocket(HandSide::Left,  draw);
        SetWeaponSocket(HandSide::Right, draw);
    }

    // 移動が終わるより前に動作を終わらせない。次の入力で行き先が上書きされると、
    // 補間の途中から別のソケットへ折り返して不自然に見える。
    const float finishAt = std::max(draw ? drawDuration : holsterDuration,
                                    transferAt + transferBlendDuration);
    if (m_actionTime >= finishAt)
        m_action = Action::None;
}

inline void WeaponRigComponent::OnDrawGizmos()
{
    if (!drawSocketGizmos) return;
    GameObject* self = scene.Self();
    if (!self) return;

    const HandSide hands[2] = { HandSide::Left, HandSide::Right };
    for (const HandSide hand : hands) {
        if (GameObject* handSocket = FindInSubtree(*self, HandSocketName(hand)))
            debug.DrawSphere(handSocket->transform.worldPosition, 0.03f,
                             { 0.2f, 1.0f, 0.4f, 1.0f });
        if (GameObject* holster = FindInSubtree(*self, HolsterSocketName(hand)))
            debug.DrawSphere(holster->transform.worldPosition, 0.03f,
                             { 1.0f, 0.5f, 0.2f, 1.0f });
    }
}

} // namespace sandbox
