/// @file    BossPolarityRigComponent.hpp
/// @brief   ボスの部位に乗った極どうしを引き合わせ、成立したらボス自身を転倒させる
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY 部位を «実際に» 引き寄せないか:
///   脚はスキンドメッシュのボーンで、AnimatorSystem が Phase::LateUpdate に位置も回転も
///   毎フレーム書き直す。Script から動かしても消えるため、引き合いを絵にするには
///   IK か専用クリップが要る。ここでは «引き合っている» を溜めの演出で見せ、
///   結果だけを既存の転倒 (Crash) へ落とす。芯が面白いかは結果の方で決まる。
///
/// WHY 転倒を BossAiComponent の激突スタンへ流すか:
///   «5 秒無防備・コア消灯・Boss_Crash モーション» は突進を壁へ誘導したとき用に
///   既に作ってある。転倒に別の状態を足すと、同じ «倒れている» が 2 系統になり、
///   復帰処理 (照射の後始末・重力・硬直の解除) を 2 箇所で持つことになる。
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
    FBZZ_GROUP("Pull")
    FBZZ_FIELD_RANGE(float, pullSeconds, 0.90f, "Windup", 0.1f, 5.0f)
    FBZZ_TOOLTIP("異極の 2 部位が揃ってから転倒するまで。ここが «見せ場» の長さで、"
                 "短いと «斬った瞬間に勝手に転んだ» になり、長いと待たされる")
    // WHY 対の距離を測るか: 部位はどれもボスの体の中にあるので既定では必ず届く。
    //     «離れた 2 本ほど大きく崩れる» を後から入れるための入口として持つ。
    FBZZ_FIELD_RANGE(float, pairRadius, 14.0f, "Pair Radius", 1.0f, 40.0f)
    FBZZ_TOOLTIP("対として成立する部位間の距離。ボスの全幅は 9m なので既定では全対が届く")
    FBZZ_FIELD_RANGE(float, pullDecay, 2.5f, "Decay", 0.5f, 20.0f)
    FBZZ_TOOLTIP("対が崩れたとき溜めが戻る速さ。実時間の倍率")

    FBZZ_GROUP("Topple")
    FBZZ_FIELD_RANGE(float, toppleSeconds, 5.0f, "Topple Seconds", 0.5f, 15.0f)
    FBZZ_TOOLTIP("転倒して無防備な秒数。既定は激突スタンと同じ 5 秒")
    FBZZ_FIELD_RANGE_INT(int, toppleSelfDamage, 40, "Self Damage", 0, 2000)
    FBZZ_TOOLTIP("転倒そのもので入るダメージ。0 にすると «隙を作るだけ» になる")

    FBZZ_GROUP("Feel")
    // 帯電した部位をデバッグ球で囲う。輪郭が入る前の仮表示なので既定では出さない。
    FBZZ_FIELD(bool, drawPartMarks, false, "Draw Part Marks")
    FBZZ_TOOLTIP("帯電している部位を極の色の球で囲う。輪郭が出ないときの確認用")
    FBZZ_FIELD_RANGE(float, markRadius, 1.00f, "Mark Radius", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, ringIntervalFar, 0.32f, "Ring Interval (start)", 0.02f, 2.0f)
    FBZZ_FIELD_RANGE(float, ringIntervalNear, 0.07f, "Ring Interval (end)", 0.02f, 2.0f)
    FBZZ_TOOLTIP("引き合いが始まってからの環の間隔。詰まっていくことで «来る» が耳と目で読める")
    FBZZ_FIELD_RANGE(float, ringRadius, 2.2f, "Ring Radius", 0.2f, 10.0f)
    FBZZ_FIELD_RANGE(float, toppleHitStop, 0.40f, "Hitstop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, toppleShake, 0.85f, "Shake", 0.0f, 1.0f)
    FBZZ_FIELD(bool, drawPullLink, true, "Draw Link")
    FBZZ_TOOLTIP("引き合っている 2 部位を線で結ぶ。帯 (BeamTrail) を入れるまでの仮表示")

    // 引き合いに使った脚は損耗する。同じ 2 本を往復させるだけで倒せると、4 本ある
    // 意味が «予備» に落ちる。使うほど選べる対が減っていく形にして、どの 2 本で
    // 組むかを毎回選び直させる。
    FBZZ_GROUP("Part Break")
    FBZZ_FIELD(bool, breakLegs, true, "Break Legs")
    FBZZ_FIELD_RANGE_INT(int, legDurability, 2, "Durability", 1, 10)
    FBZZ_TOOLTIP("脚 1 本が耐えられる引き合いの回数。転倒 1 回で 2 本が 1 ずつ減る。"
                 "2 なら 4 回の転倒で全部落ちる")
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
    FBZZ_FIELD_RANGE(float, outlinePullWidth, 1.00f, "Width (pulling)", 0.1f, 1.0f)
    FBZZ_TOOLTIP("引き合っている間の太さ。溜まるほどここへ寄る")
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

    // 引き合いを «脚が寄る» 絵にする。ボーンは AnimatorSystem が LateUpdate で毎フレーム
    // 書き直すので、IKSystem (同じ LateUpdate の後段) から動かすのが唯一の経路になる。
    FBZZ_GROUP("Leg IK")
    FBZZ_FIELD(bool, pullLegs, true, "Pull Legs")
    FBZZ_TOOLTIP("引き合っている 2 本の脚を実際に寄せる。切ると溜めの演出だけになる")
    FBZZ_FIELD_RANGE(float, pullConvergence, 0.75f, "Convergence", 0.0f, 1.0f)
    FBZZ_TOOLTIP("満溜めで «2 本の中点» までどれだけ寄せるか。1 で完全に重なるので、"
                 "脚どうしがすれ違って見える手前で止める")
    FBZZ_FIELD_RANGE(float, pullIkWeight, 1.0f, "IK Weight", 0.0f, 1.0f)
    FBZZ_TOOLTIP("IK の効き。溜め比の 2 乗に掛かるので、序盤はほとんど動かない")
    FBZZ_FIELD(bool, holdWhilePulling, true, "Hold Still")
    FBZZ_TOOLTIP("引かれている間ボスを歩かせない。接地した足を引きずるとスライドに見える")

    // IK が寄せた «先» へ、遅れと行き過ぎを持って追従させる。
    //
    // WHY IK と両方持つか: IK は目標へ補間するだけなので、寄る動きに重さが出ない
    //     ─ 到達も離脱も等速で、«引かれている» ではなく «寄せている» に見える。
    //     揺れものは IK が確定した姿勢を静止姿勢として読む後段なので、上へ重ねると
    //     «目標は IK が決め、そこへ辿り着く過程は物理が決める» になる。
    //     どちらが効いているかは Pull Legs / Spring Pull を片方ずつ切れば分かる。
    FBZZ_GROUP("Leg Spring")
    FBZZ_FIELD(bool, springPull, true, "Spring Pull")
    FBZZ_TOOLTIP("引き合いを揺れもの経由の «力» にする。切ると IK の補間だけになる")
    FBZZ_FIELD(std::string, springRootBone, "Thigh", "Root Bone")
    FBZZ_TOOLTIP("揺らし始める骨。接尾辞 (_FR など) は自動で付く。"
                 "Thigh で脚全体、Shin なら膝から下だけが振られる")
    FBZZ_FIELD_RANGE_INT(int, springDepth, 4, "Depth", 1, 8)
    FBZZ_TOOLTIP("根から何段まで揺らすか。4 で Thigh / Shin / Hock / Foot。"
                 "増やすと指まで振られる")
    FBZZ_FIELD_RANGE(float, springForce, 55.0f, "Force", 0.0f, 400.0f)
    FBZZ_TOOLTIP("満溜めで相手の脚へ向かって掛かる加速度 [m/s^2]。"
                 "溜め比の 2 乗で効くので、序盤はほとんど動かない")
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
    FBZZ_FIELD_RANGE(float, bandPullBoost, 2.5f, "Band Boost (pulling)", 1.0f, 8.0f)
    FBZZ_TOOLTIP("引き合っている間の上乗せ。溜まるほど明るくなる")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugCharged, 0, "Charged Parts")
    FBZZ_FIELD_READ_ONLY(int, debugBrokenLegs, 0, "Broken Legs")
    // 輪郭が出ないときの切り分け用。Leg Meshes が 0 なら «脚のメッシュを掴めていない»、
    // 0 でないのに Outlined が 0 なら «帯電した部位と脚の対応が取れていない»、
    // Outlined が出ているのに画面に何も無いならポストプロセス側。
    FBZZ_FIELD_READ_ONLY(int, debugLegMeshes, 0, "Leg Meshes")
    FBZZ_FIELD_READ_ONLY(int, debugOutlined, 0, "Outlined")
    FBZZ_FIELD_READ_ONLY(std::string, debugPair, "-", "Pair")
    FBZZ_FIELD_READ_ONLY(float, debugPull, 0.0f, "Pull")

    // 脚を失った状態を «その場で» 作る口。押すと引き合いで折ったときと同じ道を通る
    // ので、崩れ姿勢だけでなく AI (歩けなくなる)・当たり判定・HP バー・VFX まで
    // 本番と同じ状態になる。
    //
    // WHY 引き合わせて折るのを待たないか: 2 本折るには対を成立させて溜め切るのを
    //     2 回、しかも狙った脚で通す必要がある。崩れ方の調整に毎回それをやると、
    //     1 回の確認に数分かかって «さっきとどう変わったか» が分からなくなる。
    void DebugBreakFrontRight();
    FBZZ_BUTTON(DebugBreakFrontRight, "Break FR")
    FBZZ_TOOLTIP("Play 中に押すと、その脚を実際にもぎ取る。引き合いで折ったときと同じ"
                 "状態 (AI・判定・HP バー・VFX まで) になる。戻すには Stop → Play。"
                 "見た目だけ試すなら BossCollapsePostureComponent の Preview を使う")
    void DebugBreakFrontLeft();
    FBZZ_BUTTON(DebugBreakFrontLeft, "Break FL")
    void DebugBreakBackRight();
    FBZZ_BUTTON(DebugBreakBackRight, "Break BR")
    void DebugBreakBackLeft();
    FBZZ_BUTTON(DebugBreakBackLeft, "Break BL")

    /// 引き合いの進み [0,1]。HUD と発光が読む。
    [[nodiscard]] float PullRatio() const
    { return Clamp01(m_pull / std::max(pullSeconds, 0.01f)); }
    [[nodiscard]] bool IsPulling() const { return m_pull > 0.0f; }

    /// 脚の本数。表示側がループを回すのに使う。
    [[nodiscard]] static constexpr int LegCount() { return 4; }
    /// 脚 1 本の残り耐久 [0,1]。1 = 無傷 / 0 = 落ちた。
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
    /// 異極の対を 1 組選ぶ。見つからなければ false。
    [[nodiscard]] bool PickPair(const std::vector<Part>& parts, int& a, int& b) const;
    /// 帯電している部位を極の色で囲う。毎フレーム。
    void DrawMarks(const std::vector<Part>& parts) const;
    /// 引き合っている間だけ、間隔の詰まる環と唸りを出す。
    void TickPullFeel(const Part& a, const Part& b, float dt);
    void DrawLink(const Part& a, const Part& b) const;
    void Fire(const Part& a, const Part& b);
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
    [[nodiscard]] IKChain* EnsureChain(int leg);
    /// 脚 1 本の «アニメーションが決めた足の位置»。
    [[nodiscard]] bool FootWorld(int leg, Vector3& out) const;

    void DrivePullIk(const Part& a, const Part& b);
    /// IK を全部畳む。対が崩れたら必ず通る。
    void ReleaseIk();
    /// 脚 4 本ぶんの揺れを 1 箇所で決める。引き合いの力とよろけの力をここで足す。
    ///
    /// WHY 対の 2 本だけでなく毎フレーム 4 本を回すか: よろけは対と無関係に、
    ///     どの脚にも起きる。«引いている間だけ» の処理に混ぜると、対を組んで
    ///     いないときに斬った脚が無反応になる ── プレイヤーが最も長く過ごす
    ///     状態で手応えが消える。
    /// @param pairA / pairB 引き合っている脚 (居なければ -1)。
    void DriveSpring(int pairA, int pairB, float dt);
    /// 揺れの力・適用率・よろけの残りを 0 へ戻す。
    void ReleaseSpring();
    /// 脚 1 本ぶんの揺れチェーンの根ボーン名 ("Thigh_FR")。
    [[nodiscard]] std::string SpringChain(int leg) const
    { return springRootBone + SuffixOf(leg); }
    void DriveBands(const std::vector<Part>& parts, bool pulling) const;

    /// 引き合いに使った脚を損耗させ、限界を超えた脚をもぎ取る。
    void WearLegs(const Part& a, const Part& b);
    /// 脚を 1 本もいだ後の後始末 (崩れの申告・歩行停止・四本目の決着)。
    ///
    /// WHY もぐ処理と分けるか: «もぐ» は脚 1 本の話だが、こちらは «何本失ったか» で
    ///     決まる。デバッグから 1 本だけもいだときも同じ判定を通さないと、
    ///     ボタンで折った脚だけ崩れず歩き続ける ─ 本番と違う状態で調整することになる。
    void ApplyLegLoss();
    /// 脚 1 本を «引き合いで折れた» のと同じ手順で失わせる。
    void DebugBreakLeg(int leg);
    /// 脚 1 本を落とす。分割された `E_*_<接尾辞>` を伏せ、極を持てなくする。
    void BreakLeg(int leg, const Vector3& at);

    /// 帯電した脚のメッシュを輪郭マスクへ描く。
    void DriveOutline(const std::vector<Part>& parts, bool pulling);

    /// 脚 1 本ぶんの IK 状態。
    struct LegIk {
        EntityRef target;
        EntityRef band;
        /// その脚の当たり判定 (`HB_Hock_*`)。位置と極の問い合わせ口。
        EntityRef hitbox;
        /// その脚の分割メッシュ (`E_*_<接尾辞>`)。輪郭と欠損で名指しする。
        std::vector<EntityRef> meshes;
        /// 引き始めの足の位置。ここから中点へ寄せる。
        Vector3   anchor;
        bool      held = false;
    };

    LegIk       m_legs[4];
    /// 四本落としたときの撃破を 1 度だけ通すための札。
    /// 行動不能へ落とす処理を 1 度だけ通すための札。
    bool m_crippled = false;
    bool m_allLegsKill = false;

    /// 脚ごとの損耗。legDurability に達したらもぎ取る。
    int         m_wear[4]      = { 0, 0, 0, 0 };
    /// よろけの残り秒数と向き。斬った瞬間に入り、2 乗で減衰しながら 0 へ戻る。
    Vector3     m_flinchDir[4];
    float       m_flinchTime[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool        m_runtimeBuilt = false;
    float       m_pull         = 0.0f;
    float       m_ringTimer    = 0.0f;
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

inline IKChain* BossPolarityRigComponent::EnsureChain(int leg)
{
    GameObject* self   = scene.Self();
    GameObject* target = m_legs[leg].target.Resolve(scene);
    if (!self || !target) return nullptr;

    auto* ik = self->GetComponent<IKSolverComponent>();
    if (!ik) return nullptr;

    const int order = kChainOrderBase + leg;
    for (IKChain& chain : ik->chains) {
        if (chain.type == IKSolverType::FABRIK && chain.order == order) {
            // 的はランタイム生成なのでシーンに残らない。参照は毎回張り直す。
            chain.targetEntity = target->GetID();
            return &chain;
        }
    }

    const std::string suffix = SuffixOf(leg);
    IKChain chain{};
    chain.type  = IKSolverType::FABRIK;
    // README のリグ構成どおり 4 節。TwoBone は 3 本しか受けないので FABRIK を使う。
    chain.boneNames = { "Thigh" + suffix, "Shin" + suffix, "Hock" + suffix, "Foot" + suffix };
    chain.order        = order;
    chain.enabled      = false;
    chain.weight       = 0.0f;
    chain.targetEntity = target->GetID();
    ik->chains.push_back(std::move(chain));
    return &ik->chains.back();
}

inline bool BossPolarityRigComponent::FootWorld(int leg, Vector3& out) const
{
    const auto* rig = scene.GetScript<BossHitboxRigComponent>();
    if (!rig) return false;
    GameObject* foot = rig->FootBone(static_cast<BossLeg>(leg));
    if (!foot) return false;
    out = foot->transform.worldPosition;
    return true;
}

inline void BossPolarityRigComponent::DrivePullIk(const Part& a, const Part& b)
{
    if (!pullLegs) return;

    const int ia = LegIndexOf(a.part->legSuffix);
    const int ib = LegIndexOf(b.part->legSuffix);
    if (ia < 0 || ib < 0 || ia == ib) return;

    // 引き始めの足の位置を 1 度だけ控える。
    //
    // WHY 毎フレーム読み直さないか: IK が効き始めると足のワールド位置は «IK が動かした
    //     後» の値になる。それを次のフレームの基準にすると、自分の出力を入力に混ぜて
    //     脚がじりじり寄り続ける (溜めが 0 でも戻らなくなる)。
    const int legs[2] = { ia, ib };
    for (const int leg : legs) {
        if (m_legs[leg].held) continue;
        if (!FootWorld(leg, m_legs[leg].anchor)) return;
        m_legs[leg].held = true;
    }

    const Vector3 midpoint = (m_legs[ia].anchor + m_legs[ib].anchor) * 0.5f;
    // 序盤をほとんど動かさないために 2 乗で効かせる。線形だと «塗った瞬間から
    // ずっと脚が寄っている» に見えて、溜めの終わりが立たない。
    const float ratio = PullRatio();
    const float ease  = ratio * ratio;

    for (const int leg : legs) {
        IKChain* chain = EnsureChain(leg);
        if (!chain) continue;

        GameObject* target = m_legs[leg].target.Resolve(scene);
        if (!target) continue;

        const Vector3 goal =
            Vector3::Lerp(m_legs[leg].anchor, midpoint, ease * Clamp01(pullConvergence));
        // 的はルート直下なのでローカルとワールドが一致するが、両方書く
        // (TransformSystem が回る前に IKSystem が読む経路があるため)。
        target->transform.position      = goal;
        target->transform.worldPosition = goal;

        chain->enabled = true;
        chain->weight  = Clamp01(ease * Max(pullIkWeight, 0.0f));
    }

    if (holdWhilePulling)
        if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetRestrained(true);
}

inline void BossPolarityRigComponent::ReleaseIk()
{
    GameObject* self = scene.Self();
    if (!self) return;

    for (LegIk& leg : m_legs) leg.held = false;

    if (auto* ik = self->GetComponent<IKSolverComponent>())
        for (IKChain& chain : ik->chains)
            if (chain.type == IKSolverType::FABRIK && chain.order >= kChainOrderBase) {
                chain.enabled = false;
                chain.weight  = 0.0f;
            }

    if (auto* ai = scene.GetScript<BossAiComponent>()) ai->SetRestrained(false);
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
    if (rag && rag->IsActive() && !rag->IsStaggering()) {
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

inline void BossPolarityRigComponent::DriveSpring(int pairA, int pairB, float dt)
{
    if (!springPull) return;

    Vector3 force[4];

    // WHY 足の «今» の位置を使うか (IK の anchor ではなく): 力は «相手が今どこに
    //     居るか» で向きが決まる。控えた開始位置を向け続けると、寄っていく途中で
    //     力の向きだけが取り残され、脚が相手を通り過ぎてから曲がる。
    if (pairA >= 0 && pairB >= 0 && pairA != pairB) {
        Vector3 footA;
        Vector3 footB;
        if (FootWorld(pairA, footA) && FootWorld(pairB, footB)) {
            const float ratio = PullRatio();
            const float power = springForce * ratio * ratio;
            force[pairA] = (footB - footA).NormalizedOr(Vector3::ZERO) * power;
            force[pairB] = (footA - footB).NormalizedOr(Vector3::ZERO) * power;
        }
    }

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

inline void BossPolarityRigComponent::DriveBands(const std::vector<Part>& parts,
                                                 bool pulling) const
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
        float gain = Max(bandBrightness, 0.0f) *
                     FadeFromRemaining(part->RemainingNormalized());
        if (pulling) gain *= Lerp(1.0f, Max(bandPullBoost, 1.0f), PullRatio());

        const Vector4 base = PolarityColor(part->Current());
        material.Instance(EntityRef{ band->GetID() })
            .SetVector4(kAlbedoId, { base.x * gain, base.y * gain, base.z * gain, 1.0f });
    }
}

inline float BossPolarityRigComponent::LegDurabilityRatio(int leg) const
{
    if (leg < 0 || leg >= 4) return 0.0f;
    const int limit = std::max(legDurability, 1);
    return Clamp01(1.0f - static_cast<float>(m_wear[leg]) / static_cast<float>(limit));
}

inline bool BossPolarityRigComponent::IsLegBroken(int leg) const
{
    if (leg < 0 || leg >= 4) return true;
    return m_wear[leg] >= std::max(legDurability, 1);
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

inline void BossPolarityRigComponent::DriveOutline(const std::vector<Part>& parts,
                                                   bool pulling)
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
        float width = Clamp01(outlineWidth);
        if (pulling) width = Lerp(width, Clamp01(outlinePullWidth), PullRatio());

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

inline bool BossPolarityRigComponent::PickPair(const std::vector<Part>& parts,
                                               int& a, int& b) const
{
    const float radiusSq = std::max(pairRadius, 0.0f) * std::max(pairRadius, 0.0f);
    // WHY «残りが少ない方» を優先して選ぶか: 先に塗った側から切れていくので、
    //     組めるうちに組む対を選ばないと、対が成立した直後に片方が切れて
    //     溜めが毎回ふりだしへ戻る。
    float best = -1.0f;
    bool  found = false;

    const int count = static_cast<int>(parts.size());
    for (int i = 0; i < count; ++i) {
        if (!parts[i].part->IsCharged()) continue;
        for (int j = i + 1; j < count; ++j) {
            if (!parts[j].part->IsCharged()) continue;
            if (!IsAttracting(parts[i].part->Current(), parts[j].part->Current())) continue;
            if ((parts[j].position - parts[i].position).LengthSq() > radiusSq) continue;

            const float urgency = -std::min(parts[i].part->Remaining(),
                                            parts[j].part->Remaining());
            if (!found || urgency > best) {
                best  = urgency;
                a     = i;
                b     = j;
                found = true;
            }
        }
    }
    return found;
}

inline void BossPolarityRigComponent::DrawLink(const Part& a, const Part& b) const
{
    if (!drawPullLink) return;

    // 2 部位の色を混ぜたうえで、溜まるほど白へ寄せる。どちらの極かは両端の環が
    // 言うので、線は «どれだけ張り詰めたか» だけを担当する。
    const float   ratio = PullRatio();
    const Vector4 ca    = PolarityColor(a.part->Current());
    const Vector4 cb    = PolarityColor(b.part->Current());
    const Vector4 color{ Lerp((ca.x + cb.x) * 0.5f, 1.0f, ratio),
                         Lerp((ca.y + cb.y) * 0.5f, 1.0f, ratio),
                         Lerp((ca.z + cb.z) * 0.5f, 1.0f, ratio), 1.0f };
    debug.DrawLine(a.position, b.position, color);
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

inline void BossPolarityRigComponent::TickPullFeel(const Part& a, const Part& b, float dt)
{
    m_ringTimer -= dt;
    if (m_ringTimer > 0.0f) return;

    // 間隔が詰まっていくことで «来る» を出す。同じ間隔で刻むと、溜まっているのか
    // ただ乗っているだけなのかが音からも絵からも読めない。
    const float ratio = PullRatio();
    m_ringTimer = Lerp(std::max(ringIntervalFar, 0.02f),
                       std::max(ringIntervalNear, 0.02f), ratio);

    if (auto* rings = PolarityRingComponent::Instance()) {
        rings->Burst(a.position, ringRadius, a.part->Current());
        rings->Burst(b.position, ringRadius, b.part->Current());
    }
    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(0.10f + 0.35f * ratio, 0.05f + 0.20f * ratio, 0.06f);
    se::Play(audio, se::kAttractWindup, 0.35f + 0.65f * ratio);
}

inline void BossPolarityRigComponent::WearLegs(const Part& a, const Part& b)
{
    if (!breakLegs) return;

    const int legs[2]  = { LegIndexOf(a.part->legSuffix), LegIndexOf(b.part->legSuffix) };
    const Vector3 at[2] = { a.position, b.position };
    const int limit    = std::max(legDurability, 1);

    for (int i = 0; i < 2; ++i) {
        const int leg = legs[i];
        if (leg < 0) continue;
        if (++m_wear[leg] < limit) continue;
        BreakLeg(leg, at[i]);
    }

    ApplyLegLoss();
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
    m_legs[leg].held = false;

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

    m_wear[leg] = std::max(legDurability, 1);
    BreakLeg(leg, at);
    ApplyLegLoss();
}

inline void BossPolarityRigComponent::DebugBreakFrontRight() { DebugBreakLeg(0); }
inline void BossPolarityRigComponent::DebugBreakFrontLeft()  { DebugBreakLeg(1); }
inline void BossPolarityRigComponent::DebugBreakBackRight()  { DebugBreakLeg(2); }
inline void BossPolarityRigComponent::DebugBreakBackLeft()   { DebugBreakLeg(3); }

inline void BossPolarityRigComponent::Fire(const Part& a, const Part& b)
{
    const Vector3 midpoint = (a.position + b.position) * 0.5f;
    const Polarity shown   = a.part->Current();

    // 転倒をラグドールへ渡すのに、組んだ 2 本の足元が要る。IK を畳むと的が消えるので
    // «畳む前» に控える。取れなければ方向なしの転倒になるだけで、進行は止まらない。
    const int legA = LegIndexOf(a.part->legSuffix);
    const int legB = LegIndexOf(b.part->legSuffix);
    Vector3   footA;
    Vector3   footB;
    const bool haveFeet = legA >= 0 && legB >= 0 &&
                          FootWorld(legA, footA) && FootWorld(legB, footB);

    // 使った極は落とす。残したままだと転倒から復帰した瞬間に同じ対が再成立して、
    // プレイヤーが何もしていないのに 2 度目が始まる。
    a.part->Clear();
    b.part->Clear();
    m_pull      = 0.0f;
    m_ringTimer = 0.0f;
    // 転倒モーションが脚を持っていくので、IK は必ずここで手を離す。残すと
    // 倒れている最中も足が引き寄せ先へ引っ張られ、崩れ方が毎回違って見える。
    ReleaseIk();
    ReleaseSpring();

    if (auto* ai = scene.GetScript<BossAiComponent>())
        ai->Topple(toppleSeconds, std::max(toppleSelfDamage, 0));

    // Topple の «後» に渡す。Topple は EndAct で出しかけの行動を畳み、Boss_Crash を
    // 流し込む ── ラグドールを先に始めると、捕獲した姿勢を Animator が上書きしてから
    // 物理が走り、崩れ始めの 1 フレームだけ別のポーズが挟まる。
    if (auto* rag = scene.GetScript<BossRagdollComponent>()) {
        if (haveFeet) rag->BeginFromPair(footA, footB, toppleSeconds);
        else          rag->Begin(toppleSeconds);
    }

    if (auto* stop = HitstopManagerComponent::Instance())  stop->Hit(Clamp01(toppleHitStop));
    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(Clamp01(toppleShake));
    if (auto* pad = RumbleManagerComponent::Instance()) pad->Rumble(0.95f, 0.7f, 0.35f);
    if (auto* rings = PolarityRingComponent::Instance())
        rings->Burst(midpoint, ringRadius * 3.0f, shown);

    se::Play(audio, se::kAttractConverge);
    se::Play(audio, se::kImpactHeavy);

    // 損耗は最後。もぎ取りの止めと揺れが転倒のそれへ重なって、1 つの大きな出来事に見える。
    WearLegs(a, b);
}

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

    // 既に倒れている間は次の対を溜めない。倒れている 5 秒のあいだに組み上がると、
    // 起き上がった瞬間にもう一度転ぶ ─ プレイヤーから見て «起きない敵» になる。
    const auto* core       = scene.GetScript<BossPolarityCoreComponent>();
    const bool  isDown     = core && core->IsStaggered();

    int a = 0;
    int b = 0;
    const bool paired = !isDown && PickPair(parts, a, b);

    DrawMarks(parts);
    DriveOutline(parts, paired);

    if (paired) {
        m_pull += dt;
        DrawLink(parts[a], parts[b]);
        TickPullFeel(parts[a], parts[b], dt);
        DrivePullIk(parts[a], parts[b]);
        debugPair = ShortName(*parts[a].object) + PolaritySymbol(parts[a].part->Current()) +
                    " / " +
                    ShortName(*parts[b].object) + PolaritySymbol(parts[b].part->Current());
    } else {
        m_pull      = std::max(0.0f, m_pull - dt * std::max(pullDecay, 0.0f));
        m_ringTimer = 0.0f;
        debugPair   = "-";
        ReleaseIk();
    }

    // 揺れは対の有無に関わらず毎フレーム決める。よろけは対を組んでいなくても起きる。
    DriveSpring(paired ? LegIndexOf(parts[a].part->legSuffix) : -1,
                paired ? LegIndexOf(parts[b].part->legSuffix) : -1,
                dt);

    DriveBands(parts, paired);
    debugPull = PullRatio();

    if (paired && m_pull >= std::max(pullSeconds, 0.01f))
        Fire(parts[a], parts[b]);
}

} // namespace sandbox
