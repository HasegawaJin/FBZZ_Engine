/// @file    BossPolarityRigComponent.hpp
/// @brief   ボスの脚に極を配り、輪郭で見せ、削り切られた脚を落とす
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// 極を持つのはボスの側で、プレイヤーは «その脚に合った剣» を選ぶ。合っていれば満額
/// 通ってその脚が数秒だけ無極になり、外すと弾かれる (判定は PolarityBladeComponent)。
///
/// WHY 極を輪郭で見せるか:
///   脚は装甲が暗く、面積のわりに画面では細い。自発光を上げても «光っている» と
///   気づく前に極の色が飽和する。形の外側へ出る輪郭なら、視界の端でも
///   «どの脚が何極か» が数えられる。
///
/// WHY 脚が落ちる道を «削り切る» 1 本にするか:
///   落とし方が複数あると、どれが本筋かがプレイの中で決まらない。斬った量だけが
///   脚を落とす、という 1 本にしておけば «斬る» の結果が姿勢の変化として必ず返る。
#pragma once

#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/MeshBuilder.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Combat/BossAiComponent.hpp>
#include <Scripts/Combat/BossAnimatorComponent.hpp>
#include <Scripts/Combat/BossCollapsePostureComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossPartPolarityComponent.hpp>
#include <Scripts/Combat/BossPolarityCoreComponent.hpp>
#include <Scripts/Combat/BossRagdollComponent.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Polarity/PolarityRingComponent.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossPolarityRigComponent : public Script {
    FBZZ_SCRIPT(BossPolarityRigComponent)

public:
    // 脚ごとの極。ボスが自分で帯び、プレイヤーは «その脚に合った剣» で斬る。
    //
    // WHY 常に 2 本ずつに割るか: 4 本が同じ極になると、その間ずっと片方の剣が
    //     «外れの剣» になり、二刀で戦っている意味がその数秒だけ消える。
    //     前 2 本と後ろ 2 本で割ると、色分けが «体の前後» として一目で読める。
    FBZZ_GROUP("Part Polarity")
    FBZZ_FIELD_RANGE(float, shuffleSeconds, 7.0f, "Shuffle Every", 0.0f, 60.0f)
    FBZZ_TOOLTIP("脚の極を配り直す間隔 [秒]。前後の組が入れ替わる。0 で配り直さない")

    FBZZ_GROUP("Feel")
    // 帯電した部位をデバッグ球で囲う。輪郭が入る前の仮表示なので既定では出さない。
    FBZZ_FIELD(bool, drawPartMarks, false, "Draw Part Marks")
    FBZZ_TOOLTIP("帯電している部位を極の色の球で囲う。輪郭が出ないときの確認用")
    FBZZ_FIELD_RANGE(float, markRadius, 1.00f, "Mark Radius", 0.1f, 5.0f)

    // 脚は «削り切られたら» 落ちる。削るのは斬撃で、量は部位が持つ
    // (BossPartPolarityComponent の Max Health)。
    FBZZ_GROUP("Part Break")
    FBZZ_FIELD(bool, breakLegs, true, "Break Legs")
    FBZZ_FIELD_RANGE(float, breakHitStop, 0.30f, "Hitstop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, breakShake, 0.55f, "Shake", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE_INT(int, crippleAtBrokenLegs, 2, "Cripple At", 1, 4)
    FBZZ_TOOLTIP("この本数を失ったらボスが歩けなくなる。四足が二足になった時点で "
                 "«歩く重機» から «据え付けの砲台» へ役割が変わる")

    // 帯電した脚を輪郭で囲う。分割したおかげで «その脚のメッシュだけ» を指定できる。
    //
    // WHY 発光ではなく輪郭か: 脚は装甲が暗く、面積のわりに画面では細い。自発光を
    //     上げても «光っている» と気づく前に極の色が飽和する。形の外側へ出る輪郭なら、
    //     視界の端でも «どの脚が何極か» が数えられる (敵の帯電表示と同じ理屈)。
    FBZZ_GROUP("Leg Outline")
    FBZZ_FIELD(bool, outlineLegs, true, "Outline Legs")
    FBZZ_FIELD_RANGE(float, outlineWidth, 0.70f, "Width", 0.1f, 1.0f)
    FBZZ_TOOLTIP("ScreenEffectManager の Width に対する比。ボスの脚は大きいので "
                 "敵 (1.0) より細くしないと «輪郭» ではなく «塗り» に見える")
    // WHY 既定で遮蔽を無視するか: 敵 (小さい・全身が見える) と違い、ボスの脚は
    //     全高 6m の体の真下にある。TPS の目線では胴体・他の脚・腹下の構造に
    //     常にどこかが隠れていて、遮蔽で捨てるとマスクがほとんど残らない。
    //     «どの脚が何極か» は隠れていても読めなければ意味が無い。
    FBZZ_FIELD(bool, outlineThroughWalls, true, "Through Walls")
    FBZZ_TOOLTIP("手前に何かあっても輪郭を出す。切ると見えている面だけになる")
    // 極に関係なく 4 本すべてを常時縁取る。切り分け専用。
    //
    // WHY 要るか: «輪郭が出ない» には «申告が届いていない» と «描画が出していない» の
    //     2 通りがあり、症状がどちらも «何も見えない» で同じ。極の条件を外して
    //     出しっぱなしにすれば、出れば申告側の問題、出なければ描画側の問題と確定する。
    FBZZ_FIELD(bool, outlineAllLegs, false, "Outline All (debug)")
    FBZZ_TOOLTIP("帯電に関係なく脚 4 本を白で縁取る。出れば描画側は生きている")

    // 斬られた脚を «力» で振る層。ボーンは AnimatorSystem が LateUpdate で毎フレーム
    // 書き直すので、その後段 (SpringBoneSystem) から動かすのが唯一の経路になる。
    FBZZ_GROUP("Leg Spring")
    FBZZ_FIELD(bool, springPull, true, "Spring Pull")
    FBZZ_TOOLTIP("斬られた脚を揺れもの経由の «力» で振る。切ると脚が無反応になる")
    FBZZ_FIELD(std::string, springRootBone, "Thigh", "Root Bone")
    FBZZ_TOOLTIP("揺らし始める骨。接尾辞 (_FR など) は自動で付く。"
                 "Thigh で脚全体、Shin なら膝から下だけが振られる")
    FBZZ_FIELD_RANGE_INT(int, springDepth, 4, "Depth", 1, 8)
    FBZZ_TOOLTIP("根から何段まで揺らすか。4 で Thigh / Shin / Hock / Foot。"
                 "増やすと指まで振られる")
    FBZZ_FIELD_RANGE(float, springWeight, 1.0f, "Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("揺れの適用率。0 で FK のまま ＝ 力を掛けても動かない")
    FBZZ_FIELD_RANGE(float, springStiffness, 0.30f, "Stiffness", 0.0f, 1.0f)
    FBZZ_TOOLTIP("元の姿勢へ戻ろうとする強さ。高いほど力に逆らい、低いほど流される")
    FBZZ_FIELD_RANGE(float, springDamping, 0.45f, "Damping", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, springLimitAngle, 40.0f, "Limit Angle", 0.0f, 180.0f)
    FBZZ_TOOLTIP("元の向きから振れてよい角度。硬質の脚なので人の髪より狭く取る")

    // 斬った脚が «効いている» を返す。
    //
    // WHY 立っている間は全身を揺らさないか: 立っているボスへの斬撃はダメージが 0 で、
    //     極を乗せるためだけの手。そこで体全体がよろけると «効いた» という嘘になり、
    //     本当に効く転倒中との差が消える。立っている間は当たった脚 1 本だけが弾み、
    //     倒れている間は体そのものが押される ── 手応えの大きさが、実際の効き目と揃う。
    FBZZ_GROUP("Leg Flinch")
    FBZZ_FIELD(bool, flinchOnHit, true, "Flinch On Hit")
    FBZZ_TOOLTIP("斬った脚を弾ませる。切ると斬撃に対して脚が無反応になる")
    FBZZ_FIELD_RANGE(float, flinchSeconds, 0.32f, "Duration", 0.05f, 2.0f)
    FBZZ_TOOLTIP("弾みが収まるまで。長いと «押され続けている» に見える")
    FBZZ_FIELD_RANGE(float, flinchForce, 95.0f, "Force", 0.0f, 600.0f)
    FBZZ_TOOLTIP("当たった直後に脚へ掛かる加速度 [m/s^2]。2 乗で減衰する")
    FBZZ_FIELD_RANGE(float, flinchChargedScale, 2.2f, "Charged x", 1.0f, 6.0f)
    FBZZ_TOOLTIP("溜め斬りの倍率。溜めた時間が «重さ» として返る数少ない場所")
    FBZZ_FIELD_RANGE(float, flinchPush, 6.0f, "Ragdoll Push", 0.0f, 40.0f)
    FBZZ_TOOLTIP("転倒中に斬ったとき、当たった所を押す速さ [m/s]。"
                 "立っている間の Force とは単位が違う (あちらは加速度)")
    FBZZ_FIELD_RANGE(float, flinchPushRadius, 3.2f, "Ragdoll Radius", 0.5f, 20.0f)
    FBZZ_TOOLTIP("押しが届く半径。広げると一撃で全身が動く")
    FBZZ_FIELD_RANGE(float, staggerScale, 1.0f, "Body Stagger", 0.0f, 3.0f)
    FBZZ_TOOLTIP("斬られたときに体が泳ぐ量の倍率。角度そのものは "
                 "BossCollapsePostureComponent の Stagger > Degrees が持つ。"
                 "0 で脚だけが反応する")

    // 骨に巻く «磁力クランプ» の輪。輪郭が入ったので既定では出さない。
    //
    // WHY 消さずに残すか: 輪はジオメトリなので hdrRT に入り、明るさを 1 より上げると
    //     ブルームが拾う。輪郭 (LDR・最前面) には出せない «滲み» を足したくなったとき、
    //     ここを入れ直すのが一番安い。
    FBZZ_GROUP("Leg Band")
    FBZZ_FIELD(bool, showBands, false, "Show Bands")
    FBZZ_FIELD_FILE(bandMaterial, "Assets/Materials/Surface/Unlit.mat", "Band Material", ".mat")
    FBZZ_TOOLTIP("輪の材質。Unlit なので albedo をそのまま明るさに使える")
    FBZZ_FIELD_RANGE(float, bandRadius, 0.62f, "Band Radius", 0.05f, 3.0f)
    FBZZ_FIELD_RANGE(float, bandThickness, 0.07f, "Band Thickness", 0.01f, 0.5f)
    FBZZ_FIELD_RANGE(float, bandBrightness, 3.0f, "Band Brightness", 0.0f, 12.0f)
    FBZZ_TOOLTIP("1 を超えるとブルームが拾う。輪は面積が小さいので白飛びしにくい")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugCharged, 0, "Charged Parts")
    FBZZ_FIELD_READ_ONLY(int, debugBrokenLegs, 0, "Broken Legs")
    // 輪郭が出ないときの切り分け用。Leg Meshes が 0 なら «脚のメッシュを掴めていない»、
    // 0 でないのに Outlined が 0 なら «帯電した部位と脚の対応が取れていない»、
    // Outlined が出ているのに画面に何も無いならポストプロセス側。
    FBZZ_FIELD_READ_ONLY(int, debugLegMeshes, 0, "Leg Meshes")
    FBZZ_FIELD_READ_ONLY(int, debugOutlined, 0, "Outlined")
    FBZZ_FIELD_READ_ONLY(float, debugShuffleIn, 0.0f, "Shuffle In")

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
    [[nodiscard]] Polarity LegPolarity(int leg) const;
    /// 斬撃が部位に入った。倒れていれば体を押し、立っていればその脚だけを弾ませる。
    ///
    /// 呼ぶのは PolarityBladeComponent。極を乗せる処理と同じ場所から 1 行で呼べるよう、
    /// 部位ではなく «脚の接尾辞» を受ける (部位側は自分が何番目の脚かを知らない)。
    void Flinch(const std::string& legSuffix, const Vector3& hitPoint,
                const Vector3& direction, bool charged);

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
        BossPartPolarityComponent* part   = nullptr;
        Vector3                    position;
    };

    /// 自分の配下にある部位だけを集める。
    void CollectParts(std::vector<Part>& out) const;
    /// 脚に極を配る。打ち消されている脚は空けたまま、時間が来たら前後の組を入れ替える。
    void AssignPolarities(const std::vector<Part>& parts, float dt);
    /// 削り切られた脚を落とす。脚が落ちる道はここ 1 本。
    void BreakDepletedLegs(const std::vector<Part>& parts);
    /// 帯電している部位を極の色で囲う。毎フレーム。
    void DrawMarks(const std::vector<Part>& parts) const;
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
    /// 脚 1 本ぶんの揺れチェーンの根ボーン名 ("Thigh_FR")。
    [[nodiscard]] std::string SpringChain(int leg) const
    { return springRootBone + SuffixOf(leg); }
    void DriveBands(const std::vector<Part>& parts) const;

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
        EntityRef band;
        /// その脚の当たり判定 (`HB_Hock_*`)。位置と極の問い合わせ口。
        EntityRef hitbox;
        /// その脚の分割メッシュ (`E_*_<接尾辞>`)。輪郭と欠損で名指しする。
        std::vector<EntityRef> meshes;
    };

    LegIk       m_legs[4];
    /// 四本落としたときの撃破を 1 度だけ通すための札。
    /// 行動不能へ落とす処理を 1 度だけ通すための札。
    bool m_crippled = false;
    bool m_allLegsKill = false;

    /// もぎ取った脚の札。
    bool        m_broken[4]    = { false, false, false, false };
    /// よろけの残り秒数と向き。斬った瞬間に入り、2 乗で減衰しながら 0 へ戻る。
    Vector3     m_flinchDir[4];
    float       m_flinchTime[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool        m_runtimeBuilt = false;
    /// 脚の極を配り直すまでの残り [秒]。
    float       m_shuffle      = 0.0f;
    /// 前 2 本と後ろ 2 本、どちらが ＋ か。配り直すたびに反転する。
    bool        m_polarityFlip = false;
};

FBZZ_REFLECT(BossPolarityRigComponent)

inline std::string BossPolarityRigComponent::ShortName(const GameObject& object)
{
    const std::string& name = object.name;
    const std::size_t  cut  = name.find_last_of('_');
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

inline int BossPolarityRigComponent::LegIndexOf(const std::string& suffix)
{
    if (suffix == "_FR") return 0;
    if (suffix == "_FL") return 1;
    if (suffix == "_BR") return 2;
    if (suffix == "_BL") return 3;
    return -1;
}

inline const char* BossPolarityRigComponent::SuffixOf(int leg)
{
    static constexpr const char* kSuffix[4] = { "_FR", "_FL", "_BR", "_BL" };
    return kSuffix[std::clamp(leg, 0, 3)];
}

inline void BossPolarityRigComponent::EnsureSolver()
{
    GameObject* self = scene.Self();
    if (!self) return;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) ik = &self->AddComponent<IKSolverComponent>();
    ik->enabled = true;
}

inline void BossPolarityRigComponent::EnsureRuntime()
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

        // 輪は当たり判定の子に付ける。あの GameObject はローカル +Y が骨の向きへ
        // 揃えてあるので (BossHitboxRigComponent::AlignUpTo)、そのまま脚に巻ける。
        GameObject* owner = scene.Self();
        if (!owner || !FindInSubtree(*owner, std::string("HB_Hock") + suffix)) continue;

        const std::string bandName = "BossLegBand" + suffix;
        GameObject* band = scene.Find(bandName);
        if (!band) {
            GameObject& created = scene.Create(bandName);
            created.runtimeGenerated = true;
            band = &created;
        }
        m_legs[leg].band = EntityRef{ band->GetID() };
    }

    // 親付けと形の組み立ては «全部作り終えてから» 行う。生成ループの中で掴んだ
    // ポインタは次の Create で無効になる。
    for (int leg = 0; leg < 4; ++leg) {
        GameObject* band = m_legs[leg].band.Resolve(scene);
        GameObject* owner = scene.Self();
        if (!band || !owner) continue;

        if (GameObject* hitbox =
                FindInSubtree(*owner, std::string("HB_Hock") + SuffixOf(leg))) {
            band->SetParent(*hitbox);
            band->transform.position = Vector3::ZERO;
            band->transform.rotation = Quaternion::Identity();
        }

        if (GameObject* hitbox =
                FindInSubtree(*owner, std::string("HB_Hock") + SuffixOf(leg)))
            m_legs[leg].hitbox = EntityRef{ hitbox->GetID() };

        auto* procedural = band->GetComponent<ProceduralMeshComponent>();
        if (!procedural) procedural = &band->AddComponent<ProceduralMeshComponent>();
        if (!bandMaterial.empty()) procedural->materialPath = bandMaterial;

        MeshBuilder builder;
        builder.AddTorus(Vector3::ZERO, Vector3::UP,
                         std::max(bandRadius, 0.05f), std::max(bandThickness, 0.01f), 24, 8);
        mesh.Apply(*band, builder);
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
        debug.LogError("BossPolarityRigComponent: no split leg meshes found under the boss. "
                       "Expected direct children named E_*_FR / _FL / _BR / _BL "
                       "(see Assets/Models/Boss/README.md).");

    // 揺れチェーンは骨に直接張るので、当たり判定や分割メッシュが揃っていなくても組める。
    // 硬さと適用率は毎フレーム DriveSpring が流すので、ここでは器だけ作る。
    for (int leg = 0; leg < 4; ++leg)
        springBone.EnsureChain(SpringChain(leg), std::max(springDepth, 1));

    // 転倒の物理はこのスクリプトが持っていない。付け忘れると転倒が
    // «Boss_Crash が流れるだけ» に戻るが、これは «物理が効いていない» と
    // 画面上まったく同じに見える。組み立てのここで 1 度だけ名指しする。
    if (!scene.GetScript<BossRagdollComponent>())
        debug.LogWarning("BossPolarityRigComponent: BossRagdollComponent not found. "
                         "Topples fall back to the Boss_Crash clip. Add it to the same "
                         "GameObject as the boss AnimatorComponent.");

    m_runtimeBuilt = true;
}


inline void BossPolarityRigComponent::Flinch(const std::string& legSuffix,
                                            const Vector3& hitPoint,
                                            const Vector3& direction,
                                            bool charged)
{
    if (!flinchOnHit) return;

    const float scale = charged ? Max(flinchChargedScale, 1.0f) : 1.0f;
    auto* rag = scene.GetScript<BossRagdollComponent>();

    // 倒れているあいだは骨をラグドールが持っている。揺れものへ力を書いても
    // 後段で丸ごと上書きされるので、押す先をそちらへ振り替える。
    if (rag && rag->IsToppling()) {
        rag->PushAt(hitPoint, direction, flinchPush * scale, flinchPushRadius);
        return;
    }

    const int leg = LegIndexOf(legSuffix);
    if (leg >= 0 && !IsLegBroken(leg)) {
        m_flinchDir[leg]  = direction.NormalizedOr(Vector3::ZERO) * scale;
        m_flinchTime[leg] = Max(flinchSeconds, 0.01f);
    }

    if (staggerScale <= 0.0f) return;

    // WHY 傾ける (VisualPivot) より押す (ラグドール) を先に採るか:
    //   傾けは «決まった角度へ回して戻る» ので、どこを斬っても同じ形の動きになる。
    //   押す方は当たった場所と向きが毎回そのまま形に出るうえ、斬られた脚が先に
    //   流れて胴が遅れて付いてくる ── «押されて泳いだ» はこの遅れが作る。
    //   ラグドールを切ってあるときだけ、傾けが受け皿として残る。
    if (rag && rag->UsesStagger()) {
        rag->Stagger(hitPoint, direction, staggerScale * scale);
        return;
    }

    if (auto* posture = scene.GetScript<BossCollapsePostureComponent>())
        posture->Stagger(hitPoint, staggerScale * scale);
}

inline void BossPolarityRigComponent::DriveSpring(float dt)
{
    if (!springPull) return;

    Vector3 force[4];

    for (int leg = 0; leg < 4; ++leg) {
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

inline void BossPolarityRigComponent::ReleaseSpring()
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

inline void BossPolarityRigComponent::DriveBands(const std::vector<Part>& parts) const
{
    static constexpr MaterialPropertyId kAlbedoId{ "albedo" };

    for (int leg = 0; leg < 4; ++leg) {
        GameObject* band = m_legs[leg].band.Resolve(scene);
        if (!band) continue;

        // その脚の部位を引く。見つからない構成 (Hock が無い) では輪ごと伏せる。
        const BossPartPolarityComponent* part = nullptr;
        for (const Part& candidate : parts)
            if (LegIndexOf(candidate.part->legSuffix) == leg) { part = candidate.part; break; }

        const bool lit = showBands && part && part->IsCharged();
        band->SetActive(lit);
        if (!lit) continue;

        // 切れる直前は暗くする。«まだ乗っている» と «もう切れる» が同じ明るさだと、
        // 2 本目を入れに行くか諦めるかの判断ができない。
        const float gain = Max(bandBrightness, 0.0f) *
                           FadeFromRemaining(part->RemainingNormalized());

        const Vector4 base = PolarityColor(part->Current());
        material.Instance(EntityRef{ band->GetID() })
            .SetVector4(kAlbedoId, { base.x * gain, base.y * gain, base.z * gain, 1.0f });
    }
}

inline float BossPolarityRigComponent::LegDurabilityRatio(int leg) const
{
    if (leg < 0 || leg >= 4) return 0.0f;

    // 脚が落ちるのは «部位を削り切ったとき» なので、残りもそこから読む。
    if (GameObject* hitbox = m_legs[leg].hitbox.Resolve(scene))
        if (const auto* part = scene.GetScript<BossPartPolarityComponent>(hitbox))
            return part->IsBroken() ? 0.0f : part->HealthNormalized();

    return m_broken[leg] ? 0.0f : 1.0f;
}

inline bool BossPolarityRigComponent::IsLegBroken(int leg) const
{
    if (leg < 0 || leg >= 4) return true;
    return m_broken[leg];
}

inline Polarity BossPolarityRigComponent::LegPolarity(int leg) const
{
    if (leg < 0 || leg >= 4) return Polarity::None;
    GameObject* hitbox = m_legs[leg].hitbox.Resolve(scene);
    if (!hitbox) return Polarity::None;
    const auto* part = scene.GetScript<BossPartPolarityComponent>(hitbox);
    return part ? part->Current() : Polarity::None;
}

inline bool BossPolarityRigComponent::LegAnchor(int leg, Vector3& out) const
{
    if (leg < 0 || leg >= 4) return false;
    GameObject* hitbox = m_legs[leg].hitbox.Resolve(scene);
    if (!hitbox) return false;
    out = hitbox->transform.worldPosition;
    return true;
}

inline void BossPolarityRigComponent::DriveOutline(const std::vector<Part>& parts)
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
        if (!part.part->IsCharged()) continue;
        const int leg = LegIndexOf(part.part->legSuffix);
        if (leg < 0) continue;

        // マスクの意味は読む側 (PolarityOutline.hlsl) との取り決め:
        // RGB = 極の色 / A = 太さ。PolarityTargetComponent と同じ約束で書く。
        const Vector4 color = PolarityColor(part.part->Current());
        const float   width = Clamp01(outlineWidth);

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

inline void BossPolarityRigComponent::AssignPolarities(const std::vector<Part>& parts,
                                                       float dt)
{
    bool reshuffle = false;
    if (shuffleSeconds > 0.0f) {
        m_shuffle -= dt;
        if (m_shuffle <= 0.0f) {
            m_shuffle      = std::max(shuffleSeconds, 0.5f);
            m_polarityFlip = !m_polarityFlip;
            reshuffle      = true;
        }
    }
    debugShuffleIn = std::max(m_shuffle, 0.0f);

    for (const Part& part : parts) {
        if (!part.part->selfDriven || part.part->IsBroken()) continue;
        // 打ち消されている間は無極のまま置く。ここで塗り直すと «正しい剣で斬った»
        // 報酬が同じフレームで取り消される。
        if (part.part->IsNeutralized()) continue;
        // 既に色が付いていて配り直しでもないなら触らない。毎フレーム書くと、
        // 打ち消しから戻った瞬間の «色が戻った» が出来事として読めなくなる。
        if (!reshuffle && part.part->IsCharged()) continue;

        const int leg = LegIndexOf(part.part->legSuffix);
        if (leg < 0) continue;

        // 前 2 本 (FR/FL = 0,1) と後ろ 2 本 (BR/BL = 2,3) で割る。
        const bool plus = ((leg < 2) != m_polarityFlip);
        part.part->SetPolarity(plus ? Polarity::Plus : Polarity::Minus);
    }
}

inline void BossPolarityRigComponent::BreakDepletedLegs(const std::vector<Part>& parts)
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

inline void BossPolarityRigComponent::CollectParts(std::vector<Part>& out) const
{
    out.clear();
    GameObject* self = scene.Self();
    if (!self) return;

    for (GameObject* object : scene.FindObjectsOfType<BossPartPolarityComponent>()) {
        if (!object || !object->activeInHierarchy()) continue;
        // 盤面に複数のボスが居ても、対を組むのは自分の部位どうしだけ。
        if (BossHitboxRigComponent::BossRootOf(object) != self) continue;

        auto* part = scene.GetScript<BossPartPolarityComponent>(object);
        if (!part) continue;

        out.push_back(Part{ object, part, object->transform.worldPosition });
    }
}

inline void BossPolarityRigComponent::DrawMarks(const std::vector<Part>& parts) const
{
    if (!drawPartMarks) return;

    for (const Part& part : parts) {
        if (!part.part->IsCharged()) continue;
        // 切れる直前は薄くする。«まだ乗っている» と «もう切れる» を同じ濃さで出すと、
        // 2 本目を入れに行くか諦めるかの判断ができない。
        Vector4 color = PolarityColor(part.part->Current());
        color.w = FadeFromRemaining(part.part->RemainingNormalized());
        debug.DrawSphere(part.position, std::max(markRadius, 0.05f), color);
    }
}

inline void BossPolarityRigComponent::ApplyLegLoss()
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
            debug.LogError("BossPolarityRigComponent: BossCollapsePostureComponent が "
                           "ボスに付いていない。脚を失っても体が崩れない。");
    }

    // 四本とも落ちたら、HP がいくら残っていても倒れる。
    //
    // WHY 別のルートを用意するか: 脚を落とすのは «引き合わせてぶつける» という
    //     この作品の核をボスへ当てた手で、HP を削るのとは別の詰め方になっている。
    //     それが «動きを止めるだけ» で終わると、最後は結局どちらも斬るしかなく、
    //     機構そのものが勝ち筋にならない。四本もいだら決着、という線を引く。
    //
    // WHY 押し込み撃破に数えないか: あちらは反発で «押した» 分の軸 (game-flow.md の
    //     評価)。脚は引き合わせて壊すので、数えるなら引きの側。取り違えると
    //     ランクの 2 軸が片方へ寄る。
    if (!m_allLegsKill && broken >= LegCount()) {
        m_allLegsKill = true;
        if (auto* combat = CombatManagerComponent::Instance()) {
            if (GameObject* self = scene.Self()) {
                if (auto* health = scene.GetScript<EnemyHealthComponent>(self))
                    (void)combat->DamageEnemyDirect(self, std::max(health->Current(), 1));
            }
        }
    }
}

inline void BossPolarityRigComponent::BreakLeg(int leg, const Vector3& at)
{
    GameObject* self = scene.Self();
    if (!self) return;

    const std::string suffix = SuffixOf(leg);

    for (const EntityRef& ref : m_legs[leg].meshes) {
        if (GameObject* piece = ref.Resolve(scene)) {
            // 同じフレームで輪郭も取り下げる。次のフレームまで残ると、消えた脚の
            // 輪郭だけが 1 コマ空中に浮く。
            objectMask.Clear(*piece);
            piece->SetActive(false);
        }
    }

    // 当たり判定・極・輪・IK をまとめて畳む。絵だけ消して判定が残ると
    // «見えない脚を斬れる» になる。
    if (GameObject* hitbox = FindInSubtree(*self, std::string("HB_Hock") + suffix))
        if (auto* part = scene.GetScript<BossPartPolarityComponent>(hitbox))
            part->Break();

    if (GameObject* band = m_legs[leg].band.Resolve(scene)) band->SetActive(false);
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
        vfx->PlayImpact(at, Polarity::None, 1.0f, true);
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

inline void BossPolarityRigComponent::DebugBreakLeg(int leg)
{
    // WHY 編集中を弾くか: BreakLeg は脚のメッシュを SetActive(false) にする。停止中に
    //     押すとその状態がそのままシーンへ保存され、«最初から脚の無いボス» が
    //     ディスクに焼き付く。Play 中なら Stop で捨てられる。
    if (!app.IsPlaying()) {
        debug.LogWarning("BossPolarityRigComponent: 脚をもぐのは Play 中だけ。"
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

inline void BossPolarityRigComponent::DebugBreakFrontRight() { DebugBreakLeg(0); }
inline void BossPolarityRigComponent::DebugBreakFrontLeft()  { DebugBreakLeg(1); }
inline void BossPolarityRigComponent::DebugBreakBackRight()  { DebugBreakLeg(2); }
inline void BossPolarityRigComponent::DebugBreakBackLeft()   { DebugBreakLeg(3); }

inline void BossPolarityRigComponent::OnUpdate()
{
    const float dt = std::max(Time::deltaTime, 0.0f);

    // 生成は必ず走査より前。scene.Create が GameObject 配列を伸ばすので、
    // parts が握っているポインタを跨いで作ると途中で無効になる。
    EnsureRuntime();

    std::vector<Part> parts;
    CollectParts(parts);

    debugCharged    = 0;
    debugBrokenLegs = 0;
    for (const Part& part : parts) {
        if (part.part->IsCharged()) ++debugCharged;
        if (part.part->IsBroken())  ++debugBrokenLegs;
    }

    // 極はボスが自分で配る。プレイヤーの仕事は «その脚に合った剣を選ぶ» ことだけで、
    // 逆極どうしを引き合わせて転ばせる手は廃した。
    AssignPolarities(parts, dt);
    BreakDepletedLegs(parts);

    DrawMarks(parts);
    DriveOutline(parts);

    // よろけは斬られたときに起きる (Flinch)。当たったフレームだけ触ると押された姿勢の
    // まま止まるので、減衰しきるまで毎フレーム面倒を見る。
    DriveSpring(dt);
    DriveBands(parts);
}

} // namespace sandbox
