/// @file    WeaponRigComponent.hpp
/// @brief   双剣 2 振りの「今どこに付いているか」だけを持つ。抜く / 納める の実体。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 入力から切り離すか:
/// 以前は PlayerControllerComponent が「キーを読む」「クリップを流す」「武器の親を
/// 差し替える」を 1 つの関数でやっていた。キーバインドを変えたいだけでも付け替え
/// コードを読む羽目になり、逆に付け替えのタイミングを直すと入力の受け付け条件まで
/// 巻き込む。入力は「抜きたい」という意思の入口でしかないので、呼び出し側は
/// RequestDraw() / RequestSheathe() を呼ぶだけにし、何が起きるかは全部こちらへ閉じる。
///
/// WHY 抜身から始めるか:
/// Katana_ 接頭辞の 31 クリップは、抜刀と居合を除いて全部「両手に持ったまま」で
/// 作られている (player-motions.md)。背中に納めた状態を初期値にすると、ロコモーション
/// だけが刀を握った手の形になり、握っているものが無い絵になる。ゲーム中の既定は抜身で、
/// 納刀は導入とリザルトの見せ場だけが使う。
///
/// WHY 左右で移す時刻をずらすか:
/// 同時に背中へ動かすと二本の刃が背中で交差して貫通する。右を先に納めきってから
/// 左を動かすと刃の間隔が 41mm 確保できる ─ モーション側がそう作られているので、
/// 付け替えも同じ順序に合わせないと刀だけが先回りする。時刻は WeaponSockets.hpp。
///
/// WHY 親子を差し替えないか:
/// 付け替えは瞬間移動になる。背中から手までは 40cm 以上あるので、瞬間移動は
/// はっきり見える。エンジンの SocketAttachmentComponent は「行き先ソケット名を
/// 書き換えるだけで、旧ソケットから新ソケットへ blendDuration 秒かけて移る」ので、
/// このスクリプトは行き先を宣言するだけでよい。移動そのものはエンジンの仕事。
///
/// WHY 追従の宣言をシーン / Prefab 側に置くか:
/// スクリプトは編集中に走らないので、Play 開始時に生成する方式だと、その間だけ刀は
/// 「ソケットボーンの子に置かれた素の Transform」になる。刀の原点とグリップは 90 度
/// ずれている (Blender の Z-up→Y-up 変換がモデルのルート直下に残るため) ので、編集中の
/// 見た目と再生中の見た目が食い違い、どちらが正なのか画面から判断できなかった。
/// 宣言をシーンに保存すれば ConstraintSystem は編集中も走る (Editor は Play 判定なしで
/// LateUpdate を回している) ため、編集時と再生時が同じ 1 本の計算を通る。
#pragma once

#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class WeaponRigComponent : public Script {
    FBZZ_SCRIPT(WeaponRigComponent)

    // Animator が無ければ抜刀クリップが空振りするだけで、付け替えそのものは成立する。
    FBZZ_OPTIONAL_COMPONENT(AnimatorComponent)

public:
    FBZZ_GROUP("Blades")
    // 未設定なら WPN_Sword_L / WPN_Sword_R を名前で拾う。
    FBZZ_REF(GameObject, weaponLeft,  "Left Katana")
    FBZZ_REF(GameObject, weaponRight, "Right Katana")
    // 刀側の合わせ点。ここが手 / 背中のソケットに重なるように追従する。
    FBZZ_FIELD(std::string, gripSocketName, "SOCKET_Grip", "Grip Socket")
    FBZZ_TOOLTIP("刀の FBX 内のソケット名。空なら刀の原点を手のソケットに合わせる")
    FBZZ_FIELD(bool, startDrawn, true, "Start Drawn")
    FBZZ_TOOLTIP("Katana_ のロコモーションは «両手に持ったまま» で作られている。"
                 "切ると、握った手の形で何も持っていない絵になる")

    FBZZ_GROUP("Draw / Sheathe Animation")
    // 抜刀 / 納刀は上半身だけに流す (2026-09-04)。Base に流すと走りながら抜いた瞬間に
    // 脚が抜刀クリップの立ち姿へ切り替わり、走りが 1 秒止まる。斬撃と同じ Override
    // レイヤー (Attack = Chest 以下) へ Slot で差し込めば、脚はロコモーションのまま。
    // レイヤーの重みは BladeComponent が Slot の重みから毎フレーム流している。
    //
    // WHY 専用の «UpperBody» レイヤーを増やさないか: Attack のマスク M_Katana_Slash と
    //     M_UpperBody は骨の集合が同一 (どちらも Chest 以下を重み 1、それ以外は 0)。
    //     もう 1 枚重ねても被さる骨は変わらず、重みを毎フレーム流す口だけが増える。
    FBZZ_FIELD(std::string, weaponLayerName, "Attack", "Katana Layer")
    FBZZ_TOOLTIP("抜刀 / 納刀を Slot で流す Override レイヤー (上半身 = Chest 以下)。"
                 "空にすると Base Layer の Trigger で全身クリップへ遷移する (旧挙動) ─ "
                 "走りながら抜くと脚が止まるので、通常は空にしないこと")
    FBZZ_FIELD_FILE(drawClipFile,
        "guid:f95e9fb9af1aadd78aa62a353427cf7d|Library/Baked/9873c746d0c7f993161455bdcad44fd2/anims/Katana_Draw.anim",
        "Draw Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, drawClipName, "Katana_Draw", "Draw Clip Name")
    FBZZ_FIELD_FILE(sheatheClipFile,
        "guid:0f86f25d3e776da5040d7badc8568677|Library/Baked/bf25e66cf0b7067fb10270753895d369/anims/Katana_Sheathe.anim",
        "Sheathe Clip", ".anim,.fbx")
    FBZZ_FIELD(std::string, sheatheClipName, "Katana_Sheathe", "Sheathe Clip Name")
    FBZZ_FIELD_RANGE(float, slotFadeIn,  0.08f, "Slot Fade In",  0.0f, 0.5f)
    FBZZ_FIELD_RANGE(float, slotFadeOut, 0.18f, "Slot Fade Out", 0.0f, 0.5f)
    FBZZ_FIELD(std::string, drawTriggerName,    "Draw",    "Draw Trigger")
    FBZZ_FIELD(std::string, sheatheTriggerName, "Sheathe", "Sheathe Trigger")
    FBZZ_FIELD(std::string, drawStateName,    "KatanaDraw",    "Draw State")
    FBZZ_FIELD(std::string, sheatheStateName, "KatanaSheathe", "Sheathe State")

    FBZZ_GROUP("Timing (秒 / クリップに依存しない)")
    // 既定値は player-motions.md の FBZZ_EVENT__WeaponAttach 表そのもの。
    FBZZ_FIELD_RANGE(float, drawTransferRight,    kKatanaDrawTransferR,    "Draw Transfer (R)",    0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, drawTransferLeft,     kKatanaDrawTransferL,    "Draw Transfer (L)",    0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, sheatheTransferRight, kKatanaSheatheTransferR, "Sheathe Transfer (R)", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, sheatheTransferLeft,  kKatanaSheatheTransferL, "Sheathe Transfer (L)", 0.0f, 2.0f)
    // 移動そのものにかける時間。エンジンの SocketAttachment がこの秒数で補間する。
    FBZZ_FIELD_RANGE(float, transferBlendDuration, 0.12f, "Transfer Blend", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 にすると瞬間移動になる")
    // 動作全体の長さ。次の入力を受け付けるまでの締め切りでもある。
    FBZZ_FIELD_RANGE(float, drawDuration,    kKatanaDrawDuration,    "Draw Duration",    0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, sheatheDuration, kKatanaSheatheDuration, "Sheathe Duration", 0.05f, 3.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD(bool, drawSocketGizmos, false, "Draw Socket Gizmos")

    // 刀を手に持っているか。斬撃の可否と UI がこれを見る。
    // WHY 「動作が終わったか」ではなく「移り終わったか」で立てるか:
    //     刀が手に無いのに斬れると、空の手で斬る絵になる。
    [[nodiscard]] bool IsDrawn() const { return m_drawn; }
    // 抜き / 納めの最中。入力の二度押しと、斬撃の抑止に使う。
    [[nodiscard]] bool IsBusy()  const { return m_action != Action::None; }

    // 呼び出し側が入力を受けて呼ぶ。ここから先は入力を知らない。
    void RequestDraw()    { StartAction(true); }
    void RequestSheathe() { StartAction(false); }
    // 抜いているなら納める / 納めているなら抜く。1 キー運用へ切り替えるとき用。
    void RequestToggle()  { StartAction(!m_drawn); }

    void OnStart() override;
    void OnUpdate() override;
    void OnDrawGizmos() override;

private:
    enum class Action { None, Draw, Sheathe };

    void StartAction(bool draw);
    // 刀 1 振りぶんの追従設定。Play 開始時に 1 度だけ組む。
    void SetupAttachment(HandSide hand);
    // 行き先ソケット名を書き換えるだけ。移動はエンジンが行う。
    void SetWeaponSocket(HandSide hand, bool toHand);
    // その手の刀が移る時刻 (秒)。Inspector 側の 4 つを引き当てる。
    [[nodiscard]] float TransferDelay(HandSide hand, bool draw) const;

    [[nodiscard]] GameObject* ResolveWeapon(HandSide hand) const;
    [[nodiscard]] SocketAttachmentComponent* AttachmentOf(HandSide hand) const;

    Action m_action     = Action::None;
    float  m_actionTime = 0.0f;
    // 今回の動作で、その手の行き先をもう切り替えたか。左右で時刻が違うので 2 本持つ。
    bool   m_transferred[2] = { false, false };
    bool   m_drawn          = false;
    bool   m_warnedNoWeapon = false;
    // ResolveWeapon は const。1 度だけ警告するためのフラグなので mutable で持つ。
    mutable bool m_warnedWrongWeaponRef = false;

    [[nodiscard]] static size_t HandIndex(HandSide hand)
    {
        return hand == HandSide::Right ? 0u : 1u;
    }
};

FBZZ_REFLECT(WeaponRigComponent)

inline void WeaponRigComponent::OnStart()
{
    m_action         = Action::None;
    m_actionTime     = 0.0f;
    m_transferred[0] = false;
    m_transferred[1] = false;
    m_drawn          = startDrawn;

    SetupAttachment(HandSide::Left);
    SetupAttachment(HandSide::Right);

    // appliedSocketName がまだ空なので、エンジン側は補間せず 1 フレーム目からスナップする。
    SetWeaponSocket(HandSide::Left,  m_drawn);
    SetWeaponSocket(HandSide::Right, m_drawn);
}

inline GameObject* WeaponRigComponent::ResolveWeapon(HandSide hand) const
{
    const Ref<GameObject>& reference = hand == HandSide::Right ? weaponRight : weaponLeft;
    GameObject* referenced = reference.Get();
    if (IsSwordObject(referenced, hand))
        return referenced;

    if (referenced && !m_warnedWrongWeaponRef) {
        m_warnedWrongWeaponRef = true;
        debug.LogWarning(std::string("WeaponRigComponent: ") + SwordObjectName(hand)
            + " reference points at '" + referenced->name
            + "'. Falling back to lookup by name (clear the slot in the Inspector to silence).");
    }
    return scene.Find(SwordObjectName(hand));
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
            // 黙って抜けると「刀が原点に落ちている」だけになり、原因が見えない。
            debug.LogError("WeaponRigComponent: katana GameObject not found "
                           "(assign Left/Right Katana, or name them WPN_Sword_L / WPN_Sword_R).");
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
    // WHY target を «触らない» か (空にもしないし、代入もしない):
    //   刀はルートに置き、シーン側で target に Player を指してある。ソケットに
    //   親子付けすると刀のボーンが Player の骨の側へ紛れ込み、階層で範囲を書く
    //   Avatar Mask が刀まで拾って加算レイヤーの weight を刀へ乗せてしまう
    //   (ボーン名も RootNode / SOCKET_Grip が両側で衝突する)。
    //   ここで代入し直すと «スクリプトが走っている間だけ正しい» 状態になり、
    //   編集中と再生中で配置が食い違う。宣言はシーンが持ち、こちらは行き先の
    //   ソケット名だけを切り替える。
    attachment->localSocketName = gripSocketName;
    attachment->blendDuration   = transferBlendDuration;
    attachment->followPosition  = true;
    attachment->followRotation  = true;
    // 刀のスケールはモデルが持つ値が正。手のボーンのスケールを持ち込まない。
    attachment->followScale     = false;
}

inline void WeaponRigComponent::SetWeaponSocket(HandSide hand, bool toHand)
{
    SocketAttachmentComponent* attachment = AttachmentOf(hand);
    if (!attachment)
        return;
    // Inspector で秒数を触った直後も次の切り替えから効くようにする。
    attachment->blendDuration = transferBlendDuration;
    attachment->socketName = toHand ? KatanaHandSocketName(hand) : KatanaBackSocketName(hand);
}

inline float WeaponRigComponent::TransferDelay(HandSide hand, bool draw) const
{
    const float seconds = draw
        ? (hand == HandSide::Right ? drawTransferRight : drawTransferLeft)
        : (hand == HandSide::Right ? sheatheTransferRight : sheatheTransferLeft);
    return std::max(seconds, 0.0f);
}

inline void WeaponRigComponent::StartAction(bool draw)
{
    // 進行中は割り込ませない。抜きかけで納め始めると、クリップと追従先が別々の向きへ
    // 走り出し、どちらが正か分からない状態になる。
    if (m_action != Action::None)
        return;
    if (draw == m_drawn)
        return;

    m_action         = draw ? Action::Draw : Action::Sheathe;
    m_actionTime     = 0.0f;
    m_transferred[0] = false;
    m_transferred[1] = false;

    // WHY Trigger と PlayLayerState を混ぜないか:
    //   Animator の AnyState 遷移は「今と同じステートへの遷移」をスキップする
    //   (AnimatorSystem.cpp の TryStartTransitionScoped)。スキップされた遷移は
    //   ConsumeTriggers を通らないので、両方やると Trigger が消費されずに居残り、
    //   クリップが終わって別ステートへ抜けた次のフレームにもう一度発火する。
    //   Base Layer は Trigger だけ (クロスフェードが要る)、専用レイヤーを指定された
    //   ときはステート直指定だけ、と入口を 1 本に保つ。
    if (weaponLayerName.empty()) {
        animator.SetTrigger(draw ? drawTriggerName : sheatheTriggerName);
    } else {
        // 上半身だけ。Slot なので Attack レイヤーにステートを足さなくてよい。
        // WHY 抜刀 / 納刀の付け替え時刻を変えないか: 時刻はクリップ固有の事実で、
        //     どのレイヤーで流しても手が背中へ届く瞬間は同じ。
        const std::string& file = draw ? drawClipFile : sheatheClipFile;
        const std::string& clip = draw ? drawClipName : sheatheClipName;
        if (!file.empty())
            animator.PlaySlot(weaponLayerName, file, clip, slotFadeIn, slotFadeOut, 1.0f,
                              /*loop=*/false);
        else
            animator.PlayLayerState(weaponLayerName, draw ? drawStateName : sheatheStateName);
    }
}

inline void WeaponRigComponent::OnUpdate()
{
    if (m_action == Action::None)
        return;

    m_actionTime += std::max(Time::deltaTime, 0.0f);
    const bool draw = m_action == Action::Draw;

    float lastTransferAt = 0.0f;
    const HandSide hands[2] = { HandSide::Right, HandSide::Left };
    for (const HandSide hand : hands) {
        const float transferAt = TransferDelay(hand, draw);
        lastTransferAt = std::max(lastTransferAt, transferAt);
        bool& transferred = m_transferred[HandIndex(hand)];
        if (transferred || m_actionTime < transferAt)
            continue;
        transferred = true;
        SetWeaponSocket(hand, draw);
    }

    // 「抜いている」は両手が移った時点で立てる。片手だけ移った途中で斬れると、
    // 背中に残った側の刀で斬る絵になる。
    if (m_transferred[0] && m_transferred[1])
        m_drawn = draw;

    // 移動が終わるより前に動作を終わらせない。次の入力で行き先が上書きされると、
    // 補間の途中から別のソケットへ折り返して不自然に見える。
    const float finishAt = std::max(draw ? drawDuration : sheatheDuration,
                                    lastTransferAt + transferBlendDuration);
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
        if (GameObject* handSocket = FindInSubtree(*self, KatanaHandSocketName(hand)))
            debug.DrawSphere(handSocket->transform.worldPosition, 0.03f,
                             { 0.2f, 1.0f, 0.4f, 1.0f });
        if (GameObject* back = FindInSubtree(*self, KatanaBackSocketName(hand)))
            debug.DrawSphere(back->transform.worldPosition, 0.03f,
                             { 1.0f, 0.5f, 0.2f, 1.0f });
    }
}

} // namespace sandbox
