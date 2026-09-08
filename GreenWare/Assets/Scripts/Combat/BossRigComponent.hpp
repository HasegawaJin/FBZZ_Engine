/// @file    BossRigComponent.hpp
/// @brief   ボスの脚を輪郭で見せ、削り切られた脚を «もいで» 落とす (部位破壊)
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY 斬られた脚を輪郭で見せるか:
///   脚は装甲が暗く、面積のわりに画面では細い。自発光を上げても «光っている» と
///   気づく前に色が飽和する。形の外側へ出る輪郭なら、視界の端でも
///   «今どの脚に入ったか» が読める。
///
/// WHY 脚が落ちる道を «削り切る» 1 本にするか:
///   落とし方が複数あると、どれが本筋かがプレイの中で決まらない。斬った量だけが
///   脚を落とす、という 1 本にしておけば «斬る» の結果が姿勢の変化として必ず返る。
#pragma once

#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossCollapsePostureComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossLegDebrisComponent.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/BladeColors.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossRigComponent : public Script {
    FBZZ_SCRIPT(BossRigComponent)

public:
    // 脚は «削り切られたら» 落ちる。削るのは斬撃で、量は部位が持つ
    // (BossPartComponent の Max Health)。
    FBZZ_GROUP("Part Break")
    FBZZ_FIELD(bool, breakLegs, true, "Break Legs")
    FBZZ_FIELD_RANGE(float, breakHitStop, 0.30f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, breakShake, 0.55f, "揺れ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE_INT(int, crippleAtBrokenLegs, 2, "Cripple At", 1, 4)
    FBZZ_TOOLTIP("この本数を失ったらボスが歩けなくなる。四足が二足になった時点で "
                 "«歩く重機» から «据え付けの砲台» へ役割が変わる")
    FBZZ_FIELD_RANGE(float, toppleOnLegLossSeconds, 5.0f, "脚を失って倒れる秒数", 0.0f, 15.0f)
    FBZZ_TOOLTIP("脚を 1 本落とすたびに倒れている長さ。この間だけ蓋が開き、"
                 "脚を登ってコアを叩ける。0 で «倒れない» (登れなくなる)")

    // 斬られた脚を輪郭で囲う。分割したおかげで «その脚のメッシュだけ» を指定できる。
    FBZZ_GROUP("Leg Outline")
    FBZZ_FIELD(bool, outlineLegs, true, "Outline Legs")
    FBZZ_FIELD_RANGE(float, outlineWidth, 0.70f, "幅", 0.1f, 1.0f)
    FBZZ_TOOLTIP("ScreenEffectManager の Width に対する比。ボスの脚は大きいので "
                 "敵 (1.0) より細くしないと «輪郭» ではなく «塗り» に見える")
    // WHY 既定で遮蔽を無視するか: 敵 (小さい・全身が見える) と違い、ボスの脚は
    //     全高 6m の体の真下にある。TPS の目線では胴体・他の脚・腹下の構造に
    //     常にどこかが隠れていて、遮蔽で捨てるとマスクがほとんど残らない。
    //     «どの脚に入ったか» は隠れていても読めなければ意味が無い。
    FBZZ_FIELD(bool, outlineThroughWalls, true, "壁を透かす")
    FBZZ_TOOLTIP("手前に何かあっても輪郭を出す。切ると見えている面だけになる")
    FBZZ_FIELD_COLOR(damageFlashColor, (Vector4{ 1.0f, 0.97f, 0.90f, 1.0f }), "Damage Flash")
    FBZZ_TOOLTIP("斬られた部位の輪郭が一瞬寄る色。長さは BossPartComponent の Flash")
    FBZZ_FIELD_RANGE(float, damageFlashStrength, 1.0f, "被弾フラッシュ倍率", 0.0f, 1.0f)
    FBZZ_TOOLTIP("0 で «斬られても光らない»。輪郭をどれだけ太らせるか")
    // 斬られたかに関係なく 4 本すべてを常時縁取る。切り分け専用。
    //
    // WHY 要るか: «輪郭が出ない» には «申告が届いていない» と «描画が出していない» の
    //     2 通りがあり、症状がどちらも «何も見えない» で同じ。条件を外して
    //     出しっぱなしにすれば、出れば申告側の問題、出なければ描画側の問題と確定する。
    FBZZ_FIELD(bool, outlineAllLegs, false, "Outline All (debug)")
    FBZZ_TOOLTIP("斬られたかに関係なく脚 4 本を白で縁取る。出れば描画側は生きている")

    // 斬られた脚を «力» で振る層。ボーンは AnimatorSystem が LateUpdate で毎フレーム
    // 書き直すので、その後段 (SpringBoneSystem) から動かすのが唯一の経路になる。
    FBZZ_GROUP("Leg Spring")
    FBZZ_FIELD(bool, springPull, true, "Spring Pull")
    FBZZ_TOOLTIP("斬られた脚を揺れもの経由の «力» で振る。切ると脚が無反応になる")
    FBZZ_FIELD(std::string, springRootBone, "Thigh", "ルートボーン")
    FBZZ_TOOLTIP("揺らし始める骨。接尾辞 (_FR など) は自動で付く。"
                 "Thigh で脚全体、Shin なら膝から下だけが振られる")
    FBZZ_FIELD_RANGE_INT(int, springDepth, 4, "奥行き", 1, 8)
    FBZZ_TOOLTIP("根から何段まで揺らすか。4 で Thigh / Shin / Hock / Foot。"
                 "増やすと指まで振られる")
    FBZZ_FIELD_RANGE(float, springWeight, 1.0f, "Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("揺れの適用率。0 で FK のまま ＝ 力を掛けても動かない")
    FBZZ_FIELD_RANGE(float, springStiffness, 0.30f, "硬さ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("元の姿勢へ戻ろうとする強さ。高いほど力に逆らい、低いほど流される")
    FBZZ_FIELD_RANGE(float, springDamping, 0.45f, "減衰", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, springLimitAngle, 40.0f, "Limit Angle", 0.0f, 180.0f)
    FBZZ_TOOLTIP("元の向きから振れてよい角度。硬質の脚なので人の髪より狭く取る")

    // ── 部位破壊 (もげた脚) ─────────────────────────────────────────────────
    //
    // WHY 引きずり (揺れもの) とラグドールを捨てたか (2026-09-07):
    //   壊れた脚を本体にぶら下げたまま垂らすと、生きている脚のクリップと壊れた脚の
    //   物理が付け根で押し合い、«壊れた» ではなく «動きがおかしい» に見えた。
    //   脚は «もげて床に落ちる» 物で、本体に残す理由が無い。本体の脚メッシュは消し、
    //   同じ submesh を静的メッシュとして写した剛体 (BossLegDebrisComponent) を
    //   その場に置く。本体の 21 クリップは 1 コマも触らない。
    FBZZ_GROUP("Part Destruction")
    FBZZ_FIELD(bool, debrisEnabled, true, "Spawn Debris")
    FBZZ_TOOLTIP("もげた脚を剛体として落とす。切ると従来どおり «消える» だけ")
    FBZZ_FIELD(std::string, debrisRootBone, "Thigh", "ルートボーン")
    FBZZ_TOOLTIP("脚の付け根の骨。接尾辞 (_FR など) は自動で付く。もげた脚はこの骨の"
                 "«今の姿勢» に重ねて置かれる")
    FBZZ_FIELD_RANGE(float, debrisMass, 60.0f, "Mass", 1.0f, 500.0f)
    FBZZ_FIELD_RANGE(float, debrisKick, 5.5f, "蹴り上げ", 0.0f, 30.0f)
    FBZZ_TOOLTIP("もげた瞬間に斬った側から離れる速さ [m/s]")
    FBZZ_FIELD_RANGE(float, debrisLift, 3.5f, "浮き", 0.0f, 20.0f)
    FBZZ_TOOLTIP("上へ跳ねる速さ [m/s]。0 だとその場に崩れ落ちる")
    FBZZ_FIELD_RANGE(float, debrisSpin, 4.0f, "回転", 0.0f, 30.0f)
    FBZZ_TOOLTIP("回転の速さ [rad/s]。転がって «重い物が落ちた» になる")
    FBZZ_FIELD_RANGE(float, debrisPadding, 0.25f, "Collider Padding", 0.0f, 1.0f)
    FBZZ_TOOLTIP("骨の並びから作る箱コライダーの余白 [m]。装甲の厚みぶん")
    FBZZ_FIELD_RANGE(float, debrisDrag, 0.35f, "Drag", 0.0f, 5.0f)
    FBZZ_TOOLTIP("空気抵抗。上げるとすぐ止まって «重い» が出る")
    FBZZ_FIELD_RANGE(float, debrisRest, 2.6f, "静止", 0.0f, 15.0f)
    FBZZ_TOOLTIP("床に落ち着いてから砕けて消えるまで [秒]。盤面を汚さないために消す")
    FBZZ_FIELD_READ_ONLY(int, debugDebrisSpawned, 0, "Debris Spawned")

    // 斬った脚が «効いている» を返す。当たった脚 1 本が揺れもので弾み、体は
    // BossCollapsePostureComponent の傾け (Stagger) で泳ぐ。
    //
    // WHY 物理 (ラグドール) で押さないか: 2026-09-04 に BossRagdollComponent を撤去した。
    //     21 クリップの予兆をフレーム単位で詰めてある体に物理を重ねると、押された姿勢が
    //     予兆の絵を崩す。脚の揺れものと決まった角度の傾けなら、クリップは 1 コマも壊れない。
    FBZZ_GROUP("Leg Flinch")
    FBZZ_FIELD(bool, flinchOnHit, true, "Flinch On Hit")
    FBZZ_TOOLTIP("斬った脚を弾ませる。切ると斬撃に対して脚が無反応になる")
    FBZZ_FIELD_RANGE(float, flinchSeconds, 0.32f, "継続時間", 0.05f, 2.0f)
    FBZZ_TOOLTIP("弾みが収まるまで。長いと «押され続けている» に見える")
    FBZZ_FIELD_RANGE(float, flinchForce, 95.0f, "Force", 0.0f, 600.0f)
    FBZZ_TOOLTIP("当たった直後に脚へ掛かる加速度 [m/s^2]。2 乗で減衰する")
    FBZZ_FIELD_RANGE(float, flinchChargedScale, 2.2f, "Charged x", 1.0f, 6.0f)
    FBZZ_TOOLTIP("溜め斬りの倍率。溜めた時間が «重さ» として返る数少ない場所")
    FBZZ_FIELD_RANGE(float, staggerScale, 1.0f, "本体ののけぞり", 0.0f, 3.0f)
    FBZZ_TOOLTIP("斬られたときに体が泳ぐ量の倍率。角度そのものは "
                 "BossCollapsePostureComponent の Stagger > Degrees が持つ。"
                 "0 で脚だけが反応する")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugBrokenLegs, 0, "Broken Legs")
    // 輪郭が出ないときの切り分け用。Leg Meshes が 0 なら «脚のメッシュを掴めていない»、
    // 0 でないのに Outlined が 0 なら «斬られた部位と脚の対応が取れていない»、
    // Outlined が出ているのに画面に何も無いならポストプロセス側。
    FBZZ_FIELD_READ_ONLY(int, debugLegMeshes, 0, "Leg Meshes")
    FBZZ_FIELD_READ_ONLY(int, debugOutlined, 0, "輪郭を出す")

    // 脚を失った状態を «その場で» 作る口。押すと削り切ったときと同じ道を通るので、
    // 崩れ姿勢だけでなく AI (歩けなくなる)・当たり判定・HP バー・VFX まで
    // 本番と同じ状態になる。
    //
    // WHY 斬って削り切るのを待たないか: 脚 1 本を落とすのに数十回斬る必要があり、
    //     崩れ方の調整に毎回それをやると 1 回の確認に数分かかって
    //     «さっきとどう変わったか» が分からなくなる。
    void DebugBreakFrontRight();
    FBZZ_BUTTON(DebugBreakFrontRight, "Break FR")
    FBZZ_TOOLTIP("Play 中に押すと、その脚を実際にもぎ取る。削り切ったときと同じ"
                 "状態 (AI・判定・HP バー・VFX まで) になる。戻すには Stop → Play。"
                 "見た目だけ試すなら BossCollapsePostureComponent の Preview を使う")
    void DebugBreakFrontLeft();
    FBZZ_BUTTON(DebugBreakFrontLeft, "Break FL")
    void DebugBreakBackRight();
    FBZZ_BUTTON(DebugBreakBackRight, "Break BR")
    void DebugBreakBackLeft();
    FBZZ_BUTTON(DebugBreakBackLeft, "Break BL")

    /// 脚の本数。表示側がループを回すのに使う。
    [[nodiscard]] static constexpr int LegCount() { return 4; }
    /// 脚 1 本の残り [0,1]。1 = 無傷 / 0 = 落ちた。部位の体力をそのまま返す。
    [[nodiscard]] float LegDurabilityRatio(int leg) const;
    /// 脚がもぎ取られたか。
    [[nodiscard]] bool IsLegBroken(int leg) const;
    /// 脚に今乗っている極。乗っていなければ None。
    /// 斬撃が部位に入った。倒れていれば体を押し、立っていればその脚だけを弾ませる。
    ///
    /// 呼ぶのは BladeComponent。極を乗せる処理と同じ場所から 1 行で呼べるよう、
    /// 部位ではなく «脚の接尾辞» を受ける (部位側は自分が何番目の脚かを知らない)。
    void Flinch(const std::string& legSuffix, const Vector3& hitPoint,
                const Vector3& direction, bool charged);

    /// 倒れているボスの脚へ «とどめ» が入った。その脚をもぎ、ボスを起こす。
    /// 落とせたら true。既に無い脚・脚でない部位なら false。
    /// 呼ぶのは PlayerParryComponent (IBoss::Execute → BossCoreComponent 経由)。
    bool ExecuteLeg(GameObject* partObject, const Vector3& from);

    /// 脚 1 本の «バーを置く足場» のワールド座標。取れなければ false。
    ///
    /// WHY 表示側に骨を探させないか: 部位の当たり判定は実行時生成で、名前も階層も
    ///     BossHitboxRigComponent の都合で決まる。探し方を 2 箇所に持つと、
    ///     リグの命名を変えたときに片方だけ黙って外れる。
    [[nodiscard]] bool LegAnchor(int leg, Vector3& out) const;

    /// WHY 組み立てを開始時に済ませるか: コンポーネントの追加は ECS の格納そのものを
    ///     動かす。毎フレーム走る OnUpdate から行うと、他のシステムが巡回している
    ///     最中に配列が動きうる (PlayerHeadLookComponent と同じ理由)。
    void OnStart()  override { EnsureRuntime(); }
    void OnUpdate() override;

private:
    /// 脚 IK チェーンの order 起点。他が使っていない帯へ寄せて、脚 4 本ぶんを連番で持つ。
    static constexpr int kChainOrderBase = 700;

    /// 部位 1 つぶんの参照。位置は毎フレーム変わるので保持しない。
    struct Part {
        GameObject*                object = nullptr;
        BossPartComponent* part   = nullptr;
        Vector3                    position;
    };

    /// 自分の配下にある部位だけを集める。
    void CollectParts(std::vector<Part>& out) const;
    /// 脚に極を配る。打ち消されている脚は空けたまま、時間が来たら前後の組を入れ替える。
    /// 削り切られた脚を落とす。脚が落ちる道はここ 1 本。
    void BreakDepletedLegs(const std::vector<Part>& parts);
    /// 背のコアを削り切ったらボスを倒す。脚とは別の «削り切り» なので分けて持つ。
    void KillOnCoreDepleted(const std::vector<Part>& parts);
    /// 帯電している部位を極の色で囲う。毎フレーム。
    /// 表示用の短い部位名。"HB_Hock_FR" → "FR"。
    [[nodiscard]] static std::string ShortName(const GameObject& object);

    /// 脚の接尾辞 ("_FR") と BossLeg の番号を往復する。
    [[nodiscard]] static int         LegIndexOf(const std::string& suffix);
    [[nodiscard]] static const char* SuffixOf(int leg);

    /// GameObject を作る処理はここへ集める。
    ///
    /// WHY 1 箇所へ寄せるか: scene.Create は GameObject 配列を再確保する。部位を
    ///     走査しながら作ると、握っている GameObject* が途中で無効になる。
    ///     «作る» を先に済ませてから «読む» へ入る。
    void EnsureRuntime();
    void EnsureSolver();

    /// 脚 4 本ぶんの揺れを 1 箇所で決める。
    ///
    /// WHY 斬られた脚だけでなく毎フレーム 4 本を回すか: よろけは減衰しきるまで
    ///     数フレーム続く。当たったフレームだけ触ると、押された姿勢のまま止まる。
    void DriveSpring(float dt);
    /// 揺れの力・適用率・よろけの残りを 0 へ戻す。
    void ReleaseSpring();
    /// もげた脚を剛体として置く。本体の脚メッシュを消す前に呼ぶ (submesh を読むため)。
    void SpawnLegDebris(int leg, const Vector3& at);
    /// 脚 4 本の «バインド姿勢» を控える。もげた脚の静的メッシュをどこへ置けば
    /// 今の脚に重なるかは、これが無いと解けない。
    void CaptureBind();
    /// 脚 1 本ぶんの揺れチェーンの根ボーン名 ("Thigh_FR")。
    [[nodiscard]] std::string SpringChain(int leg) const
    { return springRootBone + SuffixOf(leg); }

    /// 脚を 1 本もいだ後の後始末 (崩れの申告・歩行停止・四本目の決着)。
    ///
    /// WHY もぐ処理と分けるか: «もぐ» は脚 1 本の話だが、こちらは «何本失ったか» で
    ///     決まる。デバッグから 1 本だけもいだときも同じ判定を通さないと、
    ///     ボタンで折った脚だけ崩れず歩き続ける ─ 本番と違う状態で調整することになる。
    void ApplyLegLoss();
    /// 脚 1 本を «削り切られた» のと同じ手順で失わせる。
    void DebugBreakLeg(int leg);
    /// 脚 1 本を落とす。分割された `E_*_<接尾辞>` を伏せ、極を持てなくする。
    void BreakLeg(int leg, const Vector3& at);

    /// 極を帯びた脚のメッシュを、その極の色で輪郭マスクへ描く。
    void DriveOutline(const std::vector<Part>& parts);

    /// 脚 1 本ぶんの IK 状態。
    struct LegIk {
        EntityRef target;
        /// その脚の当たり判定 (`HB_Hock_*`)。位置と極の問い合わせ口。
        EntityRef hitbox;
        /// その脚の分割メッシュ (`E_*_<接尾辞>`)。輪郭と欠損で名指しする。
        std::vector<EntityRef> meshes;
    };

    LegIk       m_legs[4];
    /// 四本落としたときの撃破を 1 度だけ通すための札。
    /// 行動不能へ落とす処理を 1 度だけ通すための札。
    bool m_crippled = false;
    /// コアを削り切って決着させたか。1 回きり。
    bool m_coreKill = false;

    /// もぎ取った脚の札。
    bool        m_broken[4]    = { false, false, false, false };
    /// バインド姿勢 (ボス根空間) の付け根の骨と、脚全体の箱。CaptureBind が書く。
    ///
    /// WHY 最初の 1 回だけ控えるか: 静的メッシュはバインド姿勢で描かれるので、
    ///     «バインドの付け根» がどこかを知る必要がある。最初のフレームの Script フェーズは
    ///     Animator (LateUpdate) がまだ骨を書いていないので、そこがバインドに一番近い。
    ///     DLL リロード後の再捕獲は動いている姿勢を掴む (少しずれるが壊れはしない)。
    bool        m_bindCaptured = false;
    Vector3     m_bindThighPos[4];
    Quaternion  m_bindThighRot[4];
    Vector3     m_bindLegMin[4];
    Vector3     m_bindLegMax[4];
    /// よろけの残り秒数と向き。斬った瞬間に入り、2 乗で減衰しながら 0 へ戻る。
    Vector3     m_flinchDir[4];
    float       m_flinchTime[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool        m_runtimeBuilt = false;
    /// 脚の極を配り直すまでの残り [秒]。
    /// 前 2 本と後ろ 2 本、どちらが ＋ か。配り直すたびに反転する。
};

FBZZ_REFLECT(BossRigComponent)

inline std::string BossRigComponent::ShortName(const GameObject& object)
{
    const std::string& name = object.name;
    const std::size_t  cut  = name.find_last_of('_');
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

inline int BossRigComponent::LegIndexOf(const std::string& suffix)
{
    if (suffix == "_FR") return 0;
    if (suffix == "_FL") return 1;
    if (suffix == "_BR") return 2;
    if (suffix == "_BL") return 3;
    return -1;
}

inline const char* BossRigComponent::SuffixOf(int leg)
{
    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    return kSuffix[std::clamp(leg, 0, 3)];
}

inline void BossRigComponent::EnsureSolver()
{
    GameObject* self = scene.Self();
    if (!self) return;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;
}

inline void BossRigComponent::EnsureRuntime()
{
    GameObject* self = scene.Self();
    if (!self) return;

    // WHY 毎フレーム確かめ直すか: スクリプト DLL をリロードすると Script は作り直され、
    //     EntityRef は空へ戻る。一方で作った GameObject は Scene に残っているので、
    //     «作った» を覚えたままにすると輪と的が 1 組ずつ増え続ける。名前で拾い直す。
    if (m_runtimeBuilt && m_legs[0].target.Resolve(scene)) return;

    EnsureSolver();

    // WHY ループのたびに self を引き直すか: scene.Create は GameObject 配列を再確保する。
    //     ループの外で掴んだ self は 2 本目の生成で無効になり、FindInSubtree が
    //     解放済みの階層を辿る。
    for (int leg = 0; leg < 4; ++leg) {
        const std::string suffix = SuffixOf(leg);

        const std::string targetName = "BossLegIkTarget" + suffix;
        GameObject* target = scene.Find(targetName);
        if (!target) {
            GameObject& created  = scene.Create(targetName);
            created.runtimeGenerated = true;
            target = &created;
        }
        m_legs[leg].target = EntityRef{ target->GetID() };

        GameObject* owner  = scene.Self();
        GameObject* hitbox = owner ? FindInSubtree(*owner, std::string("HB_Hock") + suffix)
                                   : nullptr;
        if (!hitbox) continue;
        m_legs[leg].hitbox = EntityRef{ hitbox->GetID() };
    }

    // 分割メッシュを脚ごとに束ねる。
    //
    // WHY Boss の «直接の子» だけを見るか: FBX の階層表現として RootNode の下にも
    //     同名のノードが居る。部分木で拾うと、描いていないノードまで輪郭と欠損の
    //     対象に入る。
    debugLegMeshes = 0;
    for (int leg = 0; leg < 4; ++leg) {
        m_legs[leg].meshes.clear();
        GameObject* owner = scene.Self();
        if (!owner) continue;

        const std::string suffix = SuffixOf(leg);
        const int childCount = owner->GetChildCount();
        for (int i = 0; i < childCount; ++i) {
            GameObject* child = owner->GetChild(i);
            if (!child) continue;
            const std::string& name = child->name;
            if (name.rfind("E_", 0) != 0) continue;
            if (name.size() <= suffix.size()) continue;
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            m_legs[leg].meshes.push_back(EntityRef{ child->GetID() });
            ++debugLegMeshes;
        }
    }

    // 0 なら «脚を 1 枚も掴めていない»。症状は «輪郭が出ない» と «脚が欠けない» の
    // 2 つだけで、どちらも黙って何も起きないので名指しで言う。
    if (debugLegMeshes == 0)
        debug.LogError("BossRigComponent: no split leg meshes found under the boss. "
                       "Expected direct children named E_*_FR / _FL / _BR / _BL "
                       "(see Assets/Models/Boss/README.md).");

    // 揺れチェーンは骨に直接張るので、当たり判定や分割メッシュが揃っていなくても組める。
    // 硬さと適用率は毎フレーム DriveSpring が流すので、ここでは器だけ作る。
    for (int leg = 0; leg < 4; ++leg)
        springBone.EnsureChain(SpringChain(leg), std::max(springDepth, 1));

    // バインド姿勢は最初の 1 回だけ (フィールドの WHY)。
    CaptureBind();

    // とどめの受け口。プレイヤーは IBoss (極) しか知らないので、極がこちらへ中継する。
    if (auto* core = scene.GetScript<BossCoreComponent>())
        core->onExecute = [this](GameObject* part, const Vector3& from) {
            return ExecuteLeg(part, from);
        };

    m_runtimeBuilt = true;
}


inline void BossRigComponent::Flinch(const std::string& legSuffix,
                                            const Vector3& hitPoint,
                                            const Vector3& direction,
                                            bool charged)
{
    if (!flinchOnHit) return;

    const float scale = charged ? Max(flinchChargedScale, 1.0f) : 1.0f;

    const int leg = LegIndexOf(legSuffix);
    if (leg >= 0 && !IsLegBroken(leg)) {
        m_flinchDir[leg]  = direction.NormalizedOr(Vector3::ZERO) * scale;
        m_flinchTime[leg] = Max(flinchSeconds, 0.01f);
    }

    if (staggerScale <= 0.0f) return;

    if (auto* posture = scene.GetScript<BossCollapsePostureComponent>())
        posture->Stagger(hitPoint, staggerScale * scale);
}

inline void BossRigComponent::DriveSpring(float dt)
{
    if (!springPull) return;

    Vector3 force[4];

    for (int leg = 0; leg < 4; ++leg) {
        // もげた脚に揺れは要らない (メッシュはもう本体に無い)。
        if (IsLegBroken(leg)) {
            const std::string chain = SpringChain(leg);
            springBone.ClearForce(chain);
            springBone.SetWeight(chain, 0.0f);
            continue;
        }

        if (m_flinchTime[leg] > 0.0f) {
            m_flinchTime[leg] = Max(0.0f, m_flinchTime[leg] - dt);
            // 減衰は 2 乗。線形だと «押されている» が終わり際まで残り、戻る動きが
            // «力に逆らって戻っている» に見える。終わりを 0 に寄せると、適用率を
            // 落とす瞬間に脚が既に静止姿勢へ着いている。
            const float remain = m_flinchTime[leg] / Max(flinchSeconds, 0.01f);
            force[leg] += m_flinchDir[leg] * (flinchForce * remain * remain);
        }

        const std::string chain = SpringChain(leg);
        const float       power = force[leg].Length();
        if (power <= EPSILON) {
            springBone.ClearForce(chain);
            springBone.SetWeight(chain, 0.0f);
            continue;
        }

        springBone.SetChainEnabled(chain, true);
        // 硬さは毎フレーム流す。EnsureRuntime は 1 度しか通らないので、あちらへ置くと
        // Play 中に Inspector で動かしても何も変わらない ── 調整のための数値が
        // 調整中だけ効かない、という一番困る形になる。
        springBone.SetSpring(chain, springStiffness, springDamping);
        springBone.SetLimitAngle(chain, springLimitAngle);
        springBone.SetWeight(chain, Clamp01(springWeight));
        springBone.SetForce(chain, force[leg], power);
    }
}

inline void BossRigComponent::ReleaseSpring()
{
    for (int leg = 0; leg < 4; ++leg) {
        m_flinchTime[leg] = 0.0f;
        const std::string chain = SpringChain(leg);
        springBone.ClearForce(chain);
        // 適用率まで落とす。力だけ切ると、溜めていたぶんが «勝手に戻る» 動きとして
        // 数フレーム残り、対が崩れた瞬間に脚が跳ねて見える。
        springBone.SetWeight(chain, 0.0f);
    }
}

inline float BossRigComponent::LegDurabilityRatio(int leg) const
{
    if (leg < 0 || leg >= 4) return 0.0f;

    // 脚が落ちるのは «部位を削り切ったとき» なので、残りもそこから読む。
    if (GameObject* hitbox = m_legs[leg].hitbox.Resolve(scene))
        if (const auto* part = scene.GetScript<BossPartComponent>(hitbox))
            return part->IsBroken() ? 0.0f : part->HealthNormalized();

    return m_broken[leg] ? 0.0f : 1.0f;
}

inline bool BossRigComponent::IsLegBroken(int leg) const
{
    if (leg < 0 || leg >= 4) return true;
    return m_broken[leg];
}

inline bool BossRigComponent::LegAnchor(int leg, Vector3& out) const
{
    if (leg < 0 || leg >= 4) return false;
    GameObject* hitbox = m_legs[leg].hitbox.Resolve(scene);
    if (!hitbox) return false;
    out = hitbox->transform.worldPosition;
    return true;
}

inline void BossRigComponent::DriveOutline(const std::vector<Part>& parts)
{
    debugOutlined = 0;
    if (!outlineLegs) return;


    bool any = false;

    if (outlineAllLegs) {
        for (int leg = 0; leg < 4; ++leg)
            for (const EntityRef& ref : m_legs[leg].meshes)
                if (GameObject* piece = ref.Resolve(scene)) {
                    objectMask.Set(*piece, { 1.0f, 1.0f, 1.0f, 1.0f }, 1.0f,
                                   true, !outlineThroughWalls);
                    ++debugOutlined;
                    any = true;
                }
        if (any)
            if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
        return;
    }
    for (const Part& part : parts) {
        // 斬られた «直後» だけ光らせる。どの脚に入ったかを返す唯一の絵になる。
        const float flash = part.part->DamageFlash();
        if (flash <= 0.0f) continue;
        const int leg = LegIndexOf(part.part->legSuffix);
        if (leg < 0) continue;

        // マスクの意味は読む側 (Outline.hlsl) との取り決め: RGB = 色 / A = 太さ。
        const float   k     = Clamp01(flash) * Clamp01(damageFlashStrength);
        const Vector4 color = Vector4{ damageFlashColor.x, damageFlashColor.y,
                                       damageFlashColor.z, 1.0f };
        const float   width = Clamp01(Lerp(Clamp01(outlineWidth), 1.0f, k));

        for (const EntityRef& ref : m_legs[leg].meshes)
            if (GameObject* piece = ref.Resolve(scene)) {
                objectMask.Set(*piece, color, width, true, !outlineThroughWalls);
                ++debugOutlined;
                any = true;
            }
    }

    // 申告はそのフレームだけ有効なので、出したいフレームは毎回パスも要求する。
    if (any)
        if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
}

inline void BossRigComponent::BreakDepletedLegs(const std::vector<Part>& parts)
{
    if (!breakLegs) return;

    for (const Part& part : parts) {
        if (part.part->IsBroken() || !part.part->IsDepleted()) continue;
        const int leg = LegIndexOf(part.part->legSuffix);
        if (leg < 0 || IsLegBroken(leg)) continue;

        // 札も一緒に倒す。IsLegBroken はこれを見ているので、書かないと
        // «メッシュは消えているのに、まだ生きている脚» になる。
        m_broken[leg] = true;
        BreakLeg(leg, part.position);
        ApplyLegLoss();
    }
}

inline void BossRigComponent::KillOnCoreDepleted(const std::vector<Part>& parts)
{
    if (m_coreKill) return;

    for (const Part& part : parts) {
        // コアは脚と違って «落ちる» 部位ではないので、接尾辞では見分けられない。
        // BossHitboxRigComponent が付ける名前で名指しする。
        if (!part.object || part.object->name != "HB_Core") continue;
        if (!part.part->IsDepleted()) continue;

        m_coreKill = true;
        part.part->Break();

        // HP を «残り全部» 削って落とす。撃破の演出・ランク・フェーズは
        // すべて EnemyHealthComponent が 0 になったところから走るので、
        // ここに別の撃破経路を作らない。
        if (auto* combat = CombatManagerComponent::Instance())
            if (GameObject* self = scene.Self())
                if (auto* health = scene.GetScript<EnemyHealthComponent>(self))
                    (void)combat->DamageEnemyDirect(self, std::max(health->Current(), 1));
        break;
    }
}

inline void BossRigComponent::CollectParts(std::vector<Part>& out) const
{
    out.clear();
    GameObject* self = scene.Self();
    if (!self) return;

    for (GameObject* object : scene.FindObjectsOfType<BossPartComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        // 盤面に複数のボスが居ても、対を組むのは自分の部位どうしだけ。
        if (BossHitboxRigComponent::BossRootOf(object) != self) continue;

        auto* part = scene.GetScript<BossPartComponent>(object);
        if (!part) continue;

        out.push_back(Part{ object, part, object->transform.worldPosition });
    }
}

inline void BossRigComponent::ApplyLegLoss()
{
    // 四足が二足になったら歩けない。判定は «壊した瞬間» に 1 度だけで、
    // 毎フレーム申告する restrained とは別物 (こちらは元に戻らない)。
    int broken = 0;
    for (int leg = 0; leg < LegCount(); ++leg)
        if (IsLegBroken(leg)) ++broken;

    int mask = 0;
    for (int leg = 0; leg < LegCount(); ++leg)
        if (IsLegBroken(leg)) mask |= (1 << leg);

    // 崩れは «2 本目を失った瞬間» で固定せず、失うたびに送り直す。3 本目が落ちれば
    // 蝶番も倒れる向きも変わる ─ 姿勢を焼いていたころの «2 本ちょうどでしか
    // 引けない» 制約はもう無い。
    if (auto* posture = scene.GetScript<BossCollapsePostureComponent>())
        posture->SetBrokenMask(mask);

    if (broken >= std::max(crippleAtBrokenLegs, 1) && !m_crippled) {
        m_crippled = true;
        if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetCrippled(true);

        // 体が崩れないと «歩けなくなった» が絵に出ない。黙って «立ったまま
        // 動かないボス» になるので、名指しで言う。
        if (!scene.GetScript<BossCollapsePostureComponent>())
            debug.LogError("BossRigComponent: BossCollapsePostureComponent が "
                           "ボスに付いていない。脚を失っても体が崩れない。");
    }

    // 脚を 1 本失うたびに倒れる。**ここが登攀の入口になる。**
    //
    // WHY 脚で «倒す» のか: 登ってコアを叩くには «ボスが止まっている数秒» が要るが、
    //     それを作る手が突進の誘導しか無いと、脚を斬った側は自分で崩しの道を塞ぐ
    //     (2 本落ちると歩けなくなり、突進が出せなくなる)。斬った結果そのものが
    //     隙になれば、プレイヤーがやったことと開いた窓が繋がる。
    //
    // WHY 四本で即撃破を止めたか: 決着をコアへ移したから (Docs/climb-core.md)。
    //     脚だけで終わる道が残っていると、コアに触らずに勝ててしまい、
    //     «登って叩く» が最後まで一度も起きない。脚は «倒す手» であって
    //     «倒しきる手» ではない。
    if (toppleOnLegLossSeconds > 0.0f)
        if (auto* ai = scene.GetScript<BossAiComponent>())
            ai->Topple(toppleOnLegLossSeconds, 0);
}

inline void BossRigComponent::BreakLeg(int leg, const Vector3& at)
{
    GameObject* self = scene.Self();
    if (!self) return;

    const std::string suffix = SuffixOf(leg);

    // 先に «物» として写してから本体の脚を消す。順番を逆にすると、消した脚から
    // submesh を読めない。
    if (debrisEnabled) SpawnLegDebris(leg, at);
    // WHY 引き直すか: SpawnLegDebris は scene.Create を何度も呼ぶ。掴んでいた self は
    //     もう指していないことがある。
    self = scene.Self();
    if (!self) return;

    for (const EntityRef& ref : m_legs[leg].meshes) {
        if (GameObject* piece = ref.Resolve(scene)) {
            // 輪郭は必ず取り下げる。壊れた脚は狙う的ではないので、残っていると
            // «まだ斬れる» と読まれる。
            objectMask.Clear(*piece);
            piece->SetActive(false);
        }
    }

    // 当たり判定・極・輪・IK をまとめて畳む。絵だけ消して判定が残ると
    // «見えない脚を斬れる» になる。
    if (GameObject* hitbox = FindInSubtree(*self, std::string("HB_Hock") + suffix))
        if (auto* part = scene.GetScript<BossPartComponent>(hitbox))
            part->Break();

    // 揺れを畳む。落ちた脚の骨へ力が残っていると、消えたメッシュの分だけ
    // 揺れものが空回りし続ける。
    ReleaseSpring();

    if (auto* ik = self->GetComponent<IKSolverComponent>())
        for (IKChain& chain : ik->chains)
            if (chain.order == kChainOrderBase + leg) {
                chain.enabled = false;
                chain.weight  = 0.0f;
            }

    // 攻撃側へ «この脚はもう無い» を渡す。踏みつけの脚選びがこれを見る。
    if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetLegBroken(leg);

    if (auto* vfx = VfxManagerComponent::Instance())
        vfx->PlayImpact(at, BladeSide::None, 1.0f, true);
    if (auto* stop = HitstopManagerComponent::Instance()) {
        stop->Hit(Clamp01(breakHitStop));
        // もげる瞬間はボスの芝居も固める。世界の止めだけだと «全部が一緒に鈍る» で
        // 終わり、脚がもげたのがボスの身に起きたことだと読み取れない。
        stop->FreezeAnimation(self, Clamp01(breakHitStop));
    }
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(Clamp01(breakShake));
    se::Play(audio, se::kImpactDebris);
    se::Play(audio, se::kEnemyDestroy);
}

inline bool BossRigComponent::ExecuteLeg(GameObject* partObject, const Vector3& from)
{
    if (!partObject) return false;
    const auto* part = scene.GetScript<BossPartComponent>(partObject);
    if (!part) return false;

    const int leg = LegIndexOf(part->legSuffix);
    if (leg < 0 || IsLegBroken(leg)) return false;

    const Vector3 at = partObject->transform.worldPosition;

    // もぐのは削り切ったときと同じ道。姿勢の崩れ・据え付け化・4 本目の決着まで揃う。
    m_broken[leg] = true;
    BreakLeg(leg, at);
    ApplyLegLoss();

    // WHY ここで起こさなくなったか (2026-09-08): 以前は «1 回の転倒で 2 本もがれる»
    //     のを防ぐために起こしていた。今は ApplyLegLoss が脚を失うたびに転倒を
    //     引き直すので、«脚 1 本 = 転倒窓 1 回» が手段によらず成り立つ。
    //     ここで起こすと、とどめで落としたときだけ窓が消えて登れなくなる。

    // 脚がもげる «重さ»。BreakLeg の止めと揺れの上に、斬った側から画面ごと引き込む。
    if (auto* screen = ScreenEffectManagerComponent::Instance())
        screen->Implode(at, 0.55f, 0.35f);
    if (auto* vfx = VfxManagerComponent::Instance()) {
        Vector3 away = at - from;
        away.y = 0.0f;
        const Vector3 flow = away.NormalizedOr(Vector3::FORWARD);
        // 切断面の «溶断» (弧・赤熱する縫い目・溶断スパーク) と、足元の土煙。
        // BreakLeg の PlayImpact (爆発) は残す ─ こちらは «脚がもげた» の重さで、
        // 溶断は «刀で斬った» の語。2 つ重ねて初めて とどめ になる。
        vfx->PlayExecute(at, flow, 1.0f);
        vfx->PlayGroundDust(at, flow, 1.0f, 1.6f);
    }
    se::Play(audio, se::kImpactHeavy);
    return true;
}

inline void BossRigComponent::DebugBreakLeg(int leg)
{
    // WHY 編集中を弾くか: BreakLeg は帯と IK を畳み、引きずらない構成では脚のメッシュを
    //     SetActive(false) にする。停止中に押すとその状態がそのままシーンへ保存され、
    //     «最初から脚の壊れたボス» がディスクに焼き付く。Play 中なら Stop で捨てられる。
    if (!app.IsPlaying()) {
        debug.LogWarning("BossRigComponent: 脚をもぐのは Play 中だけ。"
                         "停止中に押すと脚を消した状態がシーンへ保存される。");
        return;
    }
    if (leg < 0 || leg >= LegCount() || IsLegBroken(leg)) return;

    GameObject* self = scene.Self();
    if (!self) return;

    // 引き合いで折れたときは «斬った部位» が着弾点になる。ボタンからは相手が
    // 居ないので、その脚の当たり判定の位置で代用する。
    Vector3 at = self->transform.worldPosition;
    if (GameObject* hitbox =
            FindInSubtree(*self, std::string("HB_Hock") + SuffixOf(leg)))
        at = hitbox->transform.worldPosition;

    m_broken[leg] = true;
    BreakLeg(leg, at);
    ApplyLegLoss();
}

inline void BossRigComponent::DebugBreakFrontRight() { DebugBreakLeg(0); }
inline void BossRigComponent::DebugBreakFrontLeft()  { DebugBreakLeg(1); }
inline void BossRigComponent::DebugBreakBackRight()  { DebugBreakLeg(2); }
inline void BossRigComponent::DebugBreakBackLeft()   { DebugBreakLeg(3); }

inline void BossRigComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 生成は必ず走査より前。scene.Create が GameObject 配列を伸ばすので、
    // parts が握っているポインタを跨いで作ると途中で無効になる。
    EnsureRuntime();

    std::vector<Part> parts;
    CollectParts(parts);

    debugBrokenLegs = 0;
    for (const Part& part : parts)
        if (part.part->IsBroken()) ++debugBrokenLegs;

    // 進行の物差しをコア (IBoss) へ押す。バーの «LEGS 3/4» とフェーズがこれを読む。
    if (auto* core = scene.GetScript<BossCoreComponent>())
        core->SetProgress(LegCount() - debugBrokenLegs, LegCount());

    BreakDepletedLegs(parts);
    KillOnCoreDepleted(parts);
    DriveOutline(parts);

    // よろけは斬られたときに起きる (Flinch)。当たったフレームだけ触ると押された姿勢の
    // まま止まるので、減衰しきるまで毎フレーム面倒を見る。
    DriveSpring(dt);
}

// ── 部位破壊 ────────────────────────────────────────────────────────────────

inline void BossRigComponent::CaptureBind()
{
    GameObject* self = scene.Self();
    if (!self || m_bindCaptured) return;

    const Vector3    rootPos = self->transform.worldPosition;
    const Quaternion rootInv = self->transform.worldRotation.Inverse();
    // 脚 1 本の骨並び (README のリグ構成)。指は先端だけ拾えば箱は足りる。
    static constexpr const char* kBones[] = {
        "Thigh", "Shin", "Hock", "Foot", "Toe1B", "Toe2B", "Toe3B", "HeelB"
    };

    bool any = false;
    for (int leg = 0; leg < 4; ++leg) {
        const std::string suffix = SuffixOf(leg);
        GameObject* thigh = FindInSubtree(*self, debrisRootBone + suffix);
        if (!thigh) continue;
        any = true;
        m_bindThighPos[leg] = rootInv * (thigh->transform.worldPosition - rootPos);
        m_bindThighRot[leg] = (rootInv * thigh->transform.worldRotation).Normalized();

        Vector3 lo{ 1.0e9f, 1.0e9f, 1.0e9f };
        Vector3 hi{ -1.0e9f, -1.0e9f, -1.0e9f };
        for (const char* bone : kBones) {
            GameObject* node = FindInSubtree(*self, std::string(bone) + suffix);
            if (!node) continue;
            const Vector3 p = rootInv * (node->transform.worldPosition - rootPos);
            lo = Vector3{ Min(lo.x, p.x), Min(lo.y, p.y), Min(lo.z, p.z) };
            hi = Vector3{ Max(hi.x, p.x), Max(hi.y, p.y), Max(hi.z, p.z) };
        }
        m_bindLegMin[leg] = lo;
        m_bindLegMax[leg] = hi;
    }
    m_bindCaptured = any;
}

inline void BossRigComponent::SpawnLegDebris(int leg, const Vector3& at)
{
    GameObject* self = scene.Self();
    if (!self) return;
    if (!m_bindCaptured) CaptureBind();

    const std::string suffix = SuffixOf(leg);
    GameObject* thigh = FindInSubtree(*self, debrisRootBone + suffix);
    if (!thigh || !m_bindCaptured) {
        debug.LogWarning("BossRigComponent: '" + debrisRootBone + suffix +
                         "' が無いので、もげた脚を置けない (消すだけになる)。");
        return;
    }

    // 今の付け根の姿勢に、バインド姿勢の脚を重ねる。静的メッシュはモデル空間
    // (= ボス根空間のバインド) で描かれるので、根をどこへ置けば付け根が一致するかを解く。
    const Quaternion rot = (thigh->transform.worldRotation * m_bindThighRot[leg].Inverse())
                               .Normalized();
    const Vector3    pos = thigh->transform.worldPosition - rot * m_bindThighPos[leg];

    // scene.Create の前に読み終える (Create は GameObject 配列を再確保する)。
    struct Piece {
        std::string   model;
        std::uint32_t submesh = 0;
        std::string   material;
    };
    std::vector<Piece> pieces;
    for (const EntityRef& ref : m_legs[leg].meshes) {
        GameObject* piece = ref.Resolve(scene);
        if (!piece) continue;
        auto* skin = piece->GetComponent<SkinnedMeshRenderer>();
        if (!skin || skin->modelPath.empty()) continue;
        Piece entry;
        // "guid:xxx|Assets/..." の形なら、パスの側だけを使う。
        const std::size_t bar = skin->modelPath.find('|');
        entry.model   = bar == std::string::npos ? skin->modelPath : skin->modelPath.substr(bar + 1);
        entry.submesh = skin->submeshIndices.empty() ? 0u : skin->submeshIndices[0];
        if (auto* material = piece->GetComponent<MaterialComponent>())
            entry.material = material->materialPath;
        pieces.push_back(entry);
    }
    if (pieces.empty()) return;

    const std::string name = std::string("BossLegDebris") + suffix;
    const EntityRef   root{ scene.Create(name).GetID() };

    // 子を先に全部作る。作りながら root を掴み続けると、途中で無効になる。
    std::vector<EntityRef> children;
    children.reserve(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i)
        children.push_back(EntityRef{ scene.Create(name + "_" + std::to_string(i)).GetID() });

    GameObject* debris = root.Resolve(scene);
    if (!debris) return;
    debris->runtimeGenerated     = true;
    debris->transform.position   = pos;
    debris->transform.rotation   = rot;
    debris->transform.worldPosition = pos;
    debris->transform.worldRotation = rot;

    for (std::size_t i = 0; i < pieces.size(); ++i) {
        GameObject* child = children[i].Resolve(scene);
        GameObject* parent = root.Resolve(scene);
        if (!child || !parent) continue;
        child->runtimeGenerated = true;
        child->SetParent(*parent);
        child->transform.position = Vector3::ZERO;
        child->transform.rotation = Quaternion::Identity();

        auto& renderer = child->AddComponent<MeshRenderer>();
        renderer.meshPath      = pieces[i].model + ":" + std::to_string(pieces[i].submesh);
        renderer.meshPathDirty = true;
        renderer.castShadows   = true;
        if (!pieces[i].material.empty()) {
            auto& material = child->AddComponent<MaterialComponent>();
            material.SetMaterialPath(pieces[i].material);
        }
    }

    debris = root.Resolve(scene);
    if (!debris) return;

    // 箱は骨の並びから。装甲の厚みぶん余白を足す。
    const Vector3 lo   = m_bindLegMin[leg];
    const Vector3 hi   = m_bindLegMax[leg];
    const float   pad  = Max(debrisPadding, 0.0f);
    const Vector3 size{ Max(hi.x - lo.x, 0.1f) + pad * 2.0f,
                        Max(hi.y - lo.y, 0.1f) + pad * 2.0f,
                        Max(hi.z - lo.z, 0.1f) + pad * 2.0f };
    {
        auto& box = debris->AddComponent<BoxColliderComponent>();
        box.SetSize(size);
        box.center = (lo + hi) * 0.5f;
    }
    {
        RigidBodyComponent rb{};
        rb.rigidBody = std::make_unique<fbzz::physics::RigidBody>();
        rb.rigidBody->SetMass(Max(debrisMass, 1.0f));
        rb.rigidBody->SetPosition(pos);
        rb.rigidBody->SetRotation(rot);
        rb.rigidBody->m_linearDrag  = Max(debrisDrag, 0.0f);
        rb.rigidBody->m_angularDrag = Max(debrisDrag, 0.0f) * 1.5f;
        rb.rigidBody->m_useCCD      = true;
        rb.rigidBody->m_ccdRadius   = Max(Min(size.x, Min(size.y, size.z)) * 0.5f, 0.2f);
        rb.ResetPhysicsSyncState(pos, rot);
        debris->AddComponent<RigidBodyComponent>(std::move(rb));
    }

    // 斬った側から離れる向きへ蹴る。at は切断面 (斬った点) なので、脚の重心から
    // 見てその反対が «もげた向き»。
    const Vector3 center = pos + rot * ((lo + hi) * 0.5f);
    Vector3 away = center - at;
    away.y = 0.0f;
    away = away.NormalizedOr(self->transform.worldRotation * Vector3::RIGHT);
    const Vector3 velocity = away * Max(debrisKick, 0.0f) + Vector3::UP * Max(debrisLift, 0.0f);
    const Vector3 axis     = Vector3::Cross(Vector3::UP, away).NormalizedOr(Vector3::FORWARD);
    const Vector3 spin     = axis * Max(debrisSpin, 0.0f)
                           + Vector3::UP * (random.Range(-1.0f, 1.0f) * Max(debrisSpin, 0.0f) * 0.3f);

    auto& script = debris->AddScript<BossLegDebrisComponent>();
    script.restSeconds = Max(debrisRest, 0.0f);
    script.Launch(velocity, spin, Max(size.x, Max(size.y, size.z)));

    ++debugDebrisSpawned;
}

} // namespace sandbox
