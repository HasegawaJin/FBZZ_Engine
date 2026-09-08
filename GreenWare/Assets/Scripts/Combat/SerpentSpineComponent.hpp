/// @file    SerpentSpineComponent.hpp
/// @brief   29 本の骨を経路へ沿わせる。追従 (follow-the-leader) で頭から遡る
/// @author  Hasegawa Jin
/// @date    2026-08-31
///
/// WHY クリップを焼かないか (boss-serpent.md「リグ」):
///   16 口のどこへでも潜る動きはクリップでは表せない。Blender 側のスプライン IK は
///   プレビュー専用で、書き出しには入っていない (骨は 30 本のまま)。
///
/// WHY ワールド回転を直接書かないか:
///   `transform.worldRotation` は TransformSystem が毎フレーム親から計算し直す値で、
///   書いても次のフレームには消える (Transform.hpp「world 値は直接変更しない」)。
///   親から順にワールド回転を «自分で» 積み上げ、ローカルへ落として入れる ─
///   TransformSystem が回る前でも後でも同じ姿勢になる。
///
/// WHY 折れ角の判定で頭と胴を分けるか:
///   頭は 1.9 m で節 (0.80 m) の 2.4 倍長いため、同じ曲率でも頭の関節だけ余計に回る
///   (胴 11.8度 のとき頭 16.5度)。実測では首は 40 度まで噛まないので、
///   まとめて最大を採ると «限界を超えた» と誤診する。胴の上限は 12度/関節。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/SerpentBones.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Combat/SerpentPathComponent.hpp>
#include <Scripts/Utils/WeaponSockets.hpp>
#include <algorithm>
#include <cmath>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class SerpentSpineComponent : public Script {
    FBZZ_SCRIPT(SerpentSpineComponent)

public:
    FBZZ_GROUP("移動軌跡")
    FBZZ_FIELD_RANGE(float, headSpeed, 6.0f, "Head Speed", 0.0f, 30.0f)
    FBZZ_TOOLTIP("経路上を頭が進む速さ [m/s]。AI が «どこまで進むか» を指すので、"
                 "ここは «どれだけ速く» だけを決める")
    FBZZ_FIELD_RANGE(float, headAccel, 48.0f, "Head Accel", 1.0f, 400.0f)
    FBZZ_TOOLTIP("弧長方向の加速度 [m/s²]。手ごとの速さは 1.4 (構え) から 19.2 (突進) まで "
                 "14 倍の開きがあり、これが無いと状態が変わった 1 フレームで速度が跳ぶ。"
                 "48 なら 1.4 → 19.2 に 0.37 秒かかる ─ 溜め (0.7 秒) の中に収まる")
    FBZZ_FIELD_RANGE(float, rootBoneOffsetY, 0.0f, "Root Offset", -4.0f, 4.0f)
    FBZZ_TOOLTIP("鎖全体の高さの微調整。経路は床面を基準に組まれる")
    FBZZ_FIELD_RANGE(float, parkDepth, 40.0f, "Park Depth", 5.0f, 200.0f)
    FBZZ_TOOLTIP("経路がまだ無いあいだ胴を沈めておく深さ。書き出したままの姿勢は "
                 "24 m の棒が床に寝ている絵なので、開幕からアリーナを横切って見える")

    // 折れ角の上限は «首» と «胴» で別 (boss-serpent.md「曲げの上限」の実測)。
    //
    // WHY 分けないと必ず誤報するか: 頭の狙いは首から数関節へ角を «意図的に» 積む。
    //     その関節を胴の上限 (12度) で測ると、弧そのものが 11.3度 ある内輪の経路では
    //     狙った瞬間に必ず超える ─ 実際 20.4度 が出た。まとめて最大を採ると、
    //     «胴の装甲が噛んでいる» と «首を曲げた» の区別が付かない。
    FBZZ_GROUP("限界")
    FBZZ_FIELD_RANGE(float, bendLimitDegrees, 12.0f, "Body Bend Limit", 1.0f, 90.0f)
    FBZZ_TOOLTIP("胴 (Seg どうし) の装甲の輪が噛み合わない角。超えた経路は名指しで警告する")
    FBZZ_FIELD_RANGE(float, neckBendLimitDegrees, 40.0f, "Neck Bend Limit", 1.0f, 120.0f)
    FBZZ_TOOLTIP("狙いに使う首の関節 (Head Aim の Joints ぶん) の上限。実測で 40 度まで"
                 "噛まない。ここを下げると «睨む» の角度そのものが浅くなる")
    FBZZ_FIELD(bool, warnOnOverBend, true, "Warn On Over Bend")

    // 待機中の蠕動。構えたまま止まっている 24 m の胴は «置物» にしか見えない。
    FBZZ_GROUP("Undulation")
    FBZZ_FIELD_RANGE(float, undulationMeters, 0.34f, "振幅", 0.0f, 2.0f)
    FBZZ_TOOLTIP("経路に直交する水平への振れ幅。折れ角の余地に比例して自動で縮むので、"
                 "狭い経路ではここを上げてもほとんど振れない。実測では弦 8 m の経路は"
                 "弧だけで 10.6 度/関節 を使うため、ここに何を入れても 3 cm 前後までしか"
                 "出ない ─ «生きている» を背負っているのは下の Roll の方")
    FBZZ_FIELD_RANGE(float, undulationWavelength, 9.0f, "波長", 1.0f, 30.0f)
    FBZZ_TOOLTIP("波 1 つの長さ。露出 11 m に対して 9 m ならうねりが 1 山だけ乗る")
    FBZZ_FIELD_RANGE(float, undulationSpeed, 1.6f, "速さ", 0.0f, 10.0f)
    FBZZ_TOOLTIP("波が胴を流れる速さ [rad/s]。尾から頭へ流れる")
    FBZZ_FIELD_READ_ONLY(float, debugWaveClamp, 1.0f, "Amplitude Kept")
    FBZZ_TOOLTIP("Amplitude のうち実際に出せた割合。折れ角の余地で頭打ちにされた"
                 "ぶんがここに出る (弦 8 m の経路では 0.1 前後まで落ちる)")

    // 進行方向まわりの «ねじれ»。
    //
    // WHY 横揺れと別に要るか: 横揺れは経路の曲率へ直接足されるので、装甲の限界
    //     (12 度/関節) から «弧が既に使っている分» を引いた残りしか使えない。弦 8 m の
    //     経路は弧だけで 10.6 度あり、残りは 0.6 度 ─ 振幅 34 cm と書いても 3 cm しか
    //     出ず、蠕動が実質死んでいた。
    //     骨を自分の +Y (＝次の関節を向く軸) まわりに回すぶんには、関節の «折れ» は
    //     1 度も増えない。装甲の輪が波打って回るだけなので、余地をまったく食わずに
    //     «止まっていない» を出せる。
    FBZZ_GROUP("Roll")
    FBZZ_FIELD_RANGE(float, rollDegrees, 20.0f, "振幅", 0.0f, 90.0f)
    FBZZ_TOOLTIP("進行方向まわりのねじれの振幅 [deg]。折れ角の予算を使わないので"
                 "上限は装甲の見た目だけで決まる")
    FBZZ_FIELD_RANGE(float, rollWavelength, 7.0f, "波長", 1.0f, 40.0f)
    FBZZ_TOOLTIP("ねじれ 1 波の長さ [m]。露出 11 m に対して 7 m なら 1.6 波が乗る")
    FBZZ_FIELD_RANGE(float, rollSpeed, 2.2f, "速さ", 0.0f, 12.0f)
    FBZZ_TOOLTIP("波が胴を流れる速さ [rad/s]。横揺れより速くすると 2 つの波が"
                 "別々の生き物の動きに見えず、1 つのうねりとして読める")
    FBZZ_FIELD(bool, rollFadeAtMouth, true, "Fade At Mouth")
    FBZZ_TOOLTIP("口の真上ではねじれを 0 へ落とす。縦坑の壁とすれ違う所で回すと"
                 "装甲の角が縁を舐める")

    // 頭の狙い。経路は «どこを通るか» しか決めないので、誰を見ているかは別に要る。
    FBZZ_GROUP("Head Aim")
    FBZZ_FIELD_RANGE_INT(int, aimJoints, 4, "Joints", 1, 10)
    FBZZ_TOOLTIP("狙いに使う首の関節数。頭から数える。多いほど滑らかに、少ないほど鋭く向く")
    FBZZ_FIELD_RANGE(float, aimMaxPerJoint, 22.0f, "Neck Allowance", 0.0f, 40.0f)
    FBZZ_TOOLTIP("首の 1 関節目 (Head↔S01) に出してよい角。狙いはここから先に使い、"
                 "余った分だけを胴の関節へ «上限 − 弧が使っている分» を上限にこぼす。"
                 "実測の限界は 40 度だが、弧そのものの折れ (最大 11.3 度) が同じ関節に"
                 "乗るので、その分を空けて 22 度にしてある")
    FBZZ_FIELD_RANGE(float, aimResponse, 6.0f, "追従", 0.5f, 30.0f)
    FBZZ_TOOLTIP("狙いの追従の速さ。大きいほど食いつく")

    // 潰れた節は «リンク長を 0 にする» ことで畳まれる。0 を即入れると、潰した所より
    // 尾側の骨が全部その長さぶん経路上を一瞬で前へ飛ぶ (とどめ 4 節 = 3.2 m、
    // 極の折りは最大 16 m)。当たりと絵は即座に消したまま、長さだけを時間で詰める。
    FBZZ_GROUP("Crush")
    FBZZ_FIELD_RANGE(float, crushFadeSeconds, 0.20f, "フェード", 0.0f, 1.0f)
    FBZZ_TOOLTIP("潰れた節のリンク長が 0 になるまで [秒]。0 で従来どおり瞬間に詰まる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugExposed, 0, "露出")
    FBZZ_FIELD_READ_ONLY(float, debugBend, 0.0f, "Max Bend (body)")
    FBZZ_FIELD_READ_ONLY(float, debugNeckBend, 0.0f, "Max Bend (neck)")
    FBZZ_FIELD_READ_ONLY(float, debugHeadArc, 0.0f, "Head Arc")
    FBZZ_FIELD_READ_ONLY(float, debugHeadSpeed, 0.0f, "Head Speed (now)")
    FBZZ_FIELD_READ_ONLY(float, debugBodyLength, 0.0f, "Body Length")
    FBZZ_FIELD_READ_ONLY(float, debugAim, 0.0f, "Aim")
    FBZZ_FIELD(bool, drawSpine, false, "Draw Spine")

    /// 頭の弧長位置。AI が «どこまで渡ったか» を指す唯一の値。
    [[nodiscard]] float HeadArc() const { return m_headArc; }
    /// 弧長を «置き直す»。経路を張った直後の頭出しだけに使う ─ 積んだ速度も捨てる。
    void  SetHeadArc(float s) { m_headArc = s; m_headVel = 0.0f; }
    /// 目標へ寄せる。速さは headSpeed × speedScale が上限で、そこへ headAccel で
    /// 出入りする。@ret 着いていたら true。
    ///
    /// WHY 等速をやめたか: 呼ぶ側の speedScale は手ごとに 0.23 (構え) 〜 3.2 (突進) と
    ///     14 倍の開きがあり、等速だと状態が変わった 1 フレームで速度がそのまま跳ぶ。
    ///     口から出た直後 (6.0 → 1.4 m/s) と潜る瞬間 (1.4 → 8.1 m/s) が特に目立っていた。
    ///
    /// WHY 目標手前で «予測して» 減速するか: 速度に上限を掛けるだけでは、目標へ着いた
    ///     フレームで速度が 0 へ落ちる ─ 加速だけ滑らかで停止は今までどおり急になる。
    ///     残り距離から «この加速度で止まれる速さ» を出して頭打ちにすれば、
    ///     行き過ぎずに、しかも滑らかに止まる。
    bool  DriveHeadArc(float targetArc, float dt, float speedScale = 1.0f);
    /// 今の弧長方向の速さ [m/s]。«速い胴は触れると痛い» の判定がここを読む。
    [[nodiscard]] float HeadSpeedNow() const { return Abs(m_headVel); }

    /// 頭からの添字 i の関節のワールド位置 (前フレームに置いた値)。
    [[nodiscard]] Vector3 JointPosition(int headIndex) const;
    [[nodiscard]] Vector3 HeadPosition() const { return JointPosition(0); }
    /// 頭の向き。狙いが乗っていればそれも含む。
    [[nodiscard]] Vector3 HeadForward() const { return m_headForward; }

    /// 頭に «見る先» を与える。毎フレーム押し込む ─ 押されなかったフレームは
    /// 自然に経路の向きへ戻る。
    ///
    /// WHY 蛇の側でプレイヤーを探さないか: いつ狙うかは手の選択そのもの
    ///     (潜行中に頭が振り返ったら «逃げている» が読めない)。決めるのは AI 側で、
    ///     ここは «言われた方向へ、噛まない範囲で向ける» だけを持つ。
    void SetAim(const Vector3& worldPoint, float weight = 1.0f)
    {
        m_aimPoint  = worldPoint;
        m_aimWanted = Clamp01(weight);
        m_aimFresh  = true;
    }

    /// このフレームだけ蠕動を強める。押されなかったフレームは 1 倍へ戻る。
    ///
    /// WHY 振幅そのものを書かせないか: Undulation の値は «待機の生気» の調整で、
    ///     手が終わっても戻し忘れると常時それになる。押し続けている間だけ効く形なら、
    ///     状態を抜けた瞬間に自然に戻る (SetAim と同じ約束)。
    void SetUndulationScale(float scale)
    {
        m_waveWanted = Max(scale, 0.0f);
        m_waveFresh  = true;
    }

    /// その節が地上に出ているか。極を乗せられるのは出ている節だけ。
    [[nodiscard]] bool IsExposed(int headIndex) const;
    [[nodiscard]] int  ExposedCount() const { return debugExposed; }
    /// 残っている節を繋いだ長さ [m]。頭の先は含まない。
    [[nodiscard]] float BodyLength() const { return m_bodyLength; }
    /// 尾の先の弧長。ここより後ろの経路はもう誰も乗っていない。
    [[nodiscard]] float TailArc() const { return m_headArc - m_bodyLength; }
    /// 胴が完全に床下へ入っているか。
    [[nodiscard]] bool IsFullySubmerged() const { return debugExposed == 0 && !HeadIsExposed(); }
    [[nodiscard]] bool HeadIsExposed() const;

    void OnStart() override;
    void OnUpdate() override;

private:
    /// 弧が使い残した折れ角のうち、«乗せもの» 1 つが使ってよい分 [deg]。
    ///
    /// WHY 満額を配らないか: 余地を食うのは蠕動と狙いの 2 つで、どちらも自分だけが
    ///     使える前提で取ると合計で上限を越える。同じ割り前を両方へ配れば、
    ///     片方しか動いていないときも上限の内側に収まる (残りは丸めのための余白)。
    [[nodiscard]] float BendBudgetDegrees(float arcBendDegrees) const
    {
        return Max(bendLimitDegrees - arcBendDegrees, 0.0f) * 0.45f;
    }

    [[nodiscard]] SerpentPathComponent*       Path() const { return scene.GetScript<SerpentPathComponent>(); }
    [[nodiscard]] SerpentBodyComponent*       Body() const { return scene.GetScript<SerpentBodyComponent>(); }
    [[nodiscard]] SerpentHitboxRigComponent*  Rig()  const { return scene.GetScript<SerpentHitboxRigComponent>(); }

    void EnsureBones();
    void Solve();
    /// 進行方向まわりのねじれを world へ乗せる。dir は触らない。
    ///
    /// WHY dir を触らないか: ねじれは «骨が自分の +Y のまわりに回る» ことなので、
    ///     次の関節を指す向き (dir) は 1 度も変わらない ─ だからこそ折れ角の予算を
    ///     使わない。dir まで回すと、それは折れ角そのものになる。
    ///
    /// WHY 絶対角ではなく «親からの差» で積まないか: world は既に尾から積んであり、
    ///     子は祖先ぶんの回転を含んでいる。ここで各骨へ «自分の弧長で決まる角» を
    ///     直に掛ければ、後で world[i+1].Inverse() * world[i] を取ったときに
    ///     ちょうど «隣どうしの差» ＝ 1 関節ぶんのねじれだけが残る。
    void DriveRoll(Quaternion* world) const;
    /// 首を狙いへ向ける。dir / world をその場で書き換える。
    ///
    /// WHY 首へ «分けて» 掛けるか: 頭 1 本で 45 度回すと、首の 1 関節だけが
    ///     限界角を超えて装甲が噛む。同じ角を関節数で割って根元から順に積めば、
    ///     1 関節あたりは上限の内側に収まったまま合計で狙いへ届く。
    void DriveAim(Vector3* dir, Quaternion* world);
    /// 節ごとの当たりを «地上に出ていて生きている間» だけ有効にする。
    ///
    /// WHY 判定ごと畳むか: 床下の節を残すと、床の上を斬っただけで地下の節に極が乗る。
    ///     «見えているものが全部» という盤面の読み方が崩れる。
    void DriveHitboxActivity();

    EntityRef m_bones[serpent::kBoneCount];
    EntityRef m_root;
    /// 関節のワールド位置。経路から解いた値をそのまま覚える。
    Vector3   m_joints[serpent::kBoneCount]{};
    /// 骨と骨の間隔。骨 i のローカル位置の長さで、節 (i+1) に属する。
    float     m_links[serpent::kBoneCount]{};
    /// 頭からの累積弧長。潰れた節ぶんは詰まっている。
    ///
    /// WHY 保持するか: «その節が地上に出ているか» は輪郭・当たり・AI の 3 者が
    ///     別々のタイミングで訊く。訊かれるたびに 28 本ぶん足し直すと、
    ///     節が潰れた瞬間に «誰が古い長さで答えたか» で食い違う。
    float     m_offsets[serpent::kBoneCount]{};
    bool      m_offsetsValid = false;
    /// 潰れた節のリンクが残っている割合。1 = そのまま / 0 = 畳みきった。
    /// 潰れた «瞬間» を持つ必要は無い ─ IsAlive が false になった節をここが追う。
    float     m_linkFade[serpent::kBoneCount]{};
    bool      m_linkFadeValid = false;
    /// 骨が乗っている «地上の弧» の中での位置 [0,1]。床下なら負。
    /// ねじれを口の真上で落とすのに使う (Solve が毎フレーム書く)。
    float     m_surface01[serpent::kBoneCount]{};
    Vector3   m_headForward{ 0.0f, 0.0f, 1.0f };
    float     m_headArc    = 0.0f;
    /// 弧長方向の速さ [m/s]。符号は進む向き。加速度で出入りする。
    float     m_headVel    = 0.0f;
    float     m_bodyLength = 0.0f;
    bool      m_warnedBend = false;

    /// 狙い。AI が毎フレーム押し込み、押されなければ重みが 0 へ落ちる。
    Vector3   m_aimPoint{};
    float     m_aimWanted = 0.0f;
    float     m_aimWeight = 0.0f;
    bool      m_aimFresh  = false;

    /// 蠕動の倍率。締め上げの前触れなど «震える» 手が押し込む。
    float     m_waveWanted = 1.0f;
    float     m_wave       = 1.0f;
    bool      m_waveFresh  = false;
};

FBZZ_REFLECT(SerpentSpineComponent)

inline void SerpentSpineComponent::OnStart()
{
    for (EntityRef& bone : m_bones) bone = {};
    m_root       = {};
    m_headArc    = 0.0f;
    m_headVel    = 0.0f;
    m_warnedBend = false;
    for (float& fade : m_linkFade) fade = 1.0f;
    m_linkFadeValid = true;
    EnsureBones();
}

inline void SerpentSpineComponent::EnsureBones()
{
    // WHY 毎フレーム確かめ直すか: DLL リロードで Script は作り直され EntityRef は
    //     空へ戻る。骨はシーンに残っているので、名前で引き直せばそのまま続けられる。
    if (m_root.Resolve(scene) && m_bones[0].Resolve(scene)) return;

    GameObject* self = scene.Self();
    if (!self) return;

    // 骨名はシーン内で一意でない (プレイヤーにも Head / Root が居る)。部分木だけを見る。
    if (GameObject* root = FindInSubtree(*self, "Root")) m_root = EntityRef{ root->GetID() };
    for (int i = 0; i < serpent::kBoneCount; ++i)
        if (GameObject* bone = FindInSubtree(*self, serpent::BoneName(i)))
            m_bones[i] = EntityRef{ bone->GetID() };

    // 間隔は当たり判定の側が実測して持っている。2 箇所で測ると、片方だけ
    // «畳んだ後の 0» を拾ったときに胴の長さが食い違う。
    for (int i = 0; i < serpent::kBoneCount; ++i)
        m_links[i] = Rig() ? Rig()->LinkLength(i) : 0.80f;
}

inline Vector3 SerpentSpineComponent::JointPosition(int headIndex) const
{
    if (headIndex < 0 || headIndex >= serpent::kBoneCount) return Vector3::ZERO;
    return m_joints[headIndex];
}

inline bool SerpentSpineComponent::HeadIsExposed() const
{
    const auto* path = Path();
    return path && path->OnSurface(m_headArc);
}

inline bool SerpentSpineComponent::IsExposed(int headIndex) const
{
    if (headIndex == 0) return HeadIsExposed();
    if (headIndex < 1 || headIndex > serpent::kSegmentCount) return false;

    const auto* path = Path();
    if (!path || !path->Valid() || !m_offsetsValid) return false;

    // 節の «真ん中» で判定する。関節で採ると、節の胴がまだ半分床の上にあるのに
    // 当たりだけ先に消える (逆に、頭側の関節で採ると出ていない胴が斬れる)。
    const float arc = m_headArc - (m_offsets[headIndex - 1] + m_offsets[headIndex]) * 0.5f;
    return path->OnSurface(arc);
}

inline bool SerpentSpineComponent::DriveHeadArc(float targetArc, float dt, float speedScale)
{
    const float step = Max(dt, 0.0f);
    const float vmax  = Max(headSpeed, 0.0f) * Max(speedScale, 0.0f);
    const float accel = Max(headAccel, 1.0f);
    const float gap   = targetArc - m_headArc;

    // 着いた。速度も畳んでおかないと、次に別の目標を指されたとき «前の手の勢い» が残る。
    //
    // WHY 距離ではなく «この dt で埋まるか» で見るか: 到達判定を固定の距離にすると、
    //     遅い手 (構えの 1.4 m/s) では 1 フレームで埋まらない幅が «着いた» になり、
    //     速い手 (突進の 19.2 m/s) では 1 フレームで飛び越える。
    if (Abs(gap) <= Max(Abs(m_headVel) * step, 1.0e-4f) && Abs(m_headVel) <= vmax) {
        m_headArc = targetArc;
        m_headVel = 0.0f;
        debugHeadSpeed = 0.0f;
        return true;
    }

    // 残り距離から «この加速度で止まれる速さ» を出し、上限と小さい方を採る。
    // これが無いと加速だけ滑らかで、停止は今までどおり 1 フレームで 0 へ落ちる。
    const float braking = std::sqrt(2.0f * accel * Abs(gap));
    const float want    = (gap > 0.0f ? 1.0f : -1.0f) * Min(vmax, braking);

    m_headVel += Clamp(want - m_headVel, -accel * step, accel * step);
    m_headArc += m_headVel * step;
    debugHeadSpeed = Abs(m_headVel);

    // 行き過ぎたら目標で止める。丸めで 1 フレームぶん越えることがある。
    if ((gap > 0.0f && m_headArc > targetArc) || (gap < 0.0f && m_headArc < targetArc)) {
        m_headArc = targetArc;
        m_headVel = 0.0f;
        debugHeadSpeed = 0.0f;
        return true;
    }
    return false;
}

inline void SerpentSpineComponent::Solve()
{
    auto* path = Path();
    auto* body = Body();

    GameObject* root = m_root.Resolve(scene);
    if (!root) return;

    // 経路が張られる前は «床下に居ることにする»。書き出したままの休止姿勢は
    // 24 m の胴がアリーナの床に寝ている絵で、まだ起きていないボスが開幕から
    // 場を横切って見える。
    GameObject* self = scene.Self();
    if (!path || !path->Valid()) {
        GameObject* tail = m_bones[serpent::kBoneCount - 1].Resolve(scene);
        if (self && tail) {
            const Vector3 park = self->transform.worldPosition -
                                 Vector3{ 0.0f, Max(parkDepth, 5.0f), 0.0f };
            tail->transform.position =
                root->transform.worldRotation.Inverse() * (park - root->transform.worldPosition);
        }
        m_offsetsValid = false;
        debugExposed   = 0;
        return;
    }

    // 潰れた節のリンクを «時間をかけて» 0 へ畳む。
    //
    // WHY 即 0 にできないか: 胴は «頭の弧長 − 累積» で置いてあるので、リンク長が
    //     その場で 0 になると、潰した所より尾側の骨がまるごとその長さぶん経路上を
    //     1 フレームで前へ飛ぶ。とどめ (4 節) で 3.2 m、極の折り (最大 20 節超) では
    //     16 m の瞬間移動になる。当たりも絵も即座に消したまま、長さだけを詰める。
    //
    // WHY 潰れた «瞬間» を受け取らないか: 潰す側 (SerpentBodyComponent::Crush) は
    //     折り・とどめ・決着の 3 経路から呼ばれる。通知を足すと «1 経路だけ繋ぎ忘れ»
    //     が «そこだけ瞬間移動する» という形でしか出ない。IsAlive を毎フレーム見れば
    //     どの経路から潰れても同じように畳める。
    if (!m_linkFadeValid) {
        for (float& fade : m_linkFade) fade = 1.0f;
        m_linkFadeValid = true;
    }
    const float crushStep = crushFadeSeconds > 0.0f
        ? Max(Time::deltaTime, 0.0f) / crushFadeSeconds : 1.0f;
    for (int i = 0; i < serpent::kBoneCount; ++i) {
        const bool alive = !body || i == 0 || body->IsAlive(i);
        m_linkFade[i] = alive ? 1.0f : Max(m_linkFade[i] - crushStep, 0.0f);
    }

    // 頭からの累積弧長。潰れた節はリンクが 0 へ向かい、残った節が詰めて繋がる。
    float offsets[serpent::kBoneCount]{};
    offsets[0] = 0.0f;
    for (int i = 1; i < serpent::kBoneCount; ++i)
        offsets[i] = offsets[i - 1] + Max(m_links[i - 1], 0.0f) * m_linkFade[i];
    m_bodyLength = offsets[serpent::kBoneCount - 1];
    debugBodyLength = m_bodyLength;

    const float jointMeters = Max(m_links[1], 0.1f);

    // 押されなかったフレームは 1 倍へ戻す。震えは «押している間» だけの状態。
    const float waveWant = m_waveFresh ? m_waveWanted : 1.0f;
    m_waveFresh = false;
    m_wave += (waveWant - m_wave) * Clamp01(8.0f * Max(Time::deltaTime, 0.0f));
    const float amplitude = Max(undulationMeters, 0.0f) * m_wave;
    // 折れ角の余地に頭打ちされずに出せた割合。1 節でも削られたらその値を採る ─
    // «どこかで効いていない» を知りたいので、平均ではなく最悪値。
    float keptWave = 1.0f;

    for (int i = 0; i < serpent::kBoneCount; ++i) {
        m_offsets[i] = offsets[i];
        const float arc = m_headArc - offsets[i];
        Vector3 point = path->At(arc);

        // 待機中も «生きている» を出す。経路に直交する水平へ、進行波を乗せる。
        //
        // WHY 弧の端で 0 へ落とすか: 口の真上で横へ振ると、胴が開口の縁を舐めて
        //     羽や床スラブへ刺さる。出入口では振らず、弧の中ほどで最大にする。
        //     床下では振らない ─ 見えないうえに縦坑の壁を抜ける。
        //
        // WHY 振幅を «その節が乗っている弧» で測り直すか: 弦 8 m の経路は弧だけで
        //     11.2度/関節 あり、上限 12度 まで 0.8度 しか残っていない。渡っている
        //     最中は胴が 2 本の弧に跨るので、経路 1 本ぶんの余地で全身に掛けると
        //     狭い側の弧でだけ装甲が噛む。
        float local  = 0.0f;
        float radius = 0.0f;
        m_surface01[i] = -1.0f;
        if (path->SurfaceLocal(arc, &local, &radius)) {
            m_surface01[i] = local;
            if (amplitude > 0.0f) {
                const float bend = SerpentPathComponent::BendPerJointDegrees(radius, jointMeters);
                // 波が足す折れ角は «振幅 × (2π/波長)² × 関節長»。残っている余地から
                // 乗せてよい振幅を逆に出す。
                //
                // WHY 比例で縮めるだけでは足りないか: 余地に «比例» させても、余地が
                //     0.7 度しか無い内輪の経路 (弧だけで 11.3 度) では 0.4 度ぶん乗って
                //     上限を越える。足す角そのものを余地で頭打ちにすれば越えようがない。
                const float k      = TWO_PI / Max(undulationWavelength, 0.5f);
                const float budget = ToRad(BendBudgetDegrees(bend));
                const float allow  = budget / Max(jointMeters * k * k, 1.0e-4f);
                keptWave = Min(keptWave, Clamp01(allow / Max(amplitude, 1.0e-4f)));
                const float wave = Min(amplitude, allow) * std::sin(PI * local);
                if (wave > 0.0f) {
                    const Vector3 tangent = path->Tangent(arc);
                    const Vector3 lateral = Vector3::Cross(Vector3::UP, tangent)
                                                .NormalizedOr(Vector3{ 1.0f, 0.0f, 0.0f });
                    const float phase = TWO_PI * arc / Max(undulationWavelength, 0.5f)
                                      - Time::time * undulationSpeed;
                    point = point + lateral * (wave * std::sin(phase));
                }
            }
        }
        m_joints[i] = point;
    }
    m_offsetsValid = true;
    debugWaveClamp = keptWave;

    // 骨の +Y が «次の関節» を向く。潰れて重なった関節では 1 つ手前の向きを継ぐ。
    Vector3 dir[serpent::kBoneCount];
    // 頭は «次の関節» を持たないので経路の接線を使う。蠕動を乗せた後は、
    // 直後の関節との差の方が実際の胴の向きに合う。
    const Vector3 headDelta = m_joints[0] - m_joints[1];
    dir[0] = headDelta.LengthSq() > 1.0e-8f
        ? headDelta.NormalizedOr(path->Tangent(m_headArc))
        : path->Tangent(m_headArc);
    for (int i = 1; i < serpent::kBoneCount; ++i) {
        const Vector3 delta = m_joints[i - 1] - m_joints[i];
        dir[i] = delta.LengthSq() > 1.0e-8f ? delta.NormalizedOr(dir[i - 1]) : dir[i - 1];
    }

    // ワールド回転を尾から積む。関節ごとに独立に解くとねじれが毎フレーム跳ねる。
    Quaternion world[serpent::kBoneCount];
    const int last = serpent::kBoneCount - 1;
    world[last] = serpent::AlignUpTo(dir[last]);
    for (int i = last - 1; i >= 0; --i)
        world[i] = (serpent::ShortestArc(dir[i + 1], dir[i]) * world[i + 1]).Normalized();

    DriveRoll(world);
    DriveAim(dir, world);
    m_headForward = dir[0];

    // 尾の骨だけは «経路の上の点» を直に指す。ここから先は親子のローカルで決まる。
    const Quaternion rootInverse = root->transform.worldRotation.Inverse();
    const Vector3    tail        = m_joints[last] + Vector3{ 0.0f, rootBoneOffsetY, 0.0f };
    if (GameObject* bone = m_bones[last].Resolve(scene)) {
        bone->transform.position = rootInverse * (tail - root->transform.worldPosition);
        bone->transform.rotation = (rootInverse * world[last]).Normalized();
    }

    for (int i = last - 1; i >= 0; --i) {
        GameObject* bone = m_bones[i].Resolve(scene);
        if (!bone) continue;
        // 長さは累積 (offsets) と同じ畳み方でなければならない。片方だけ即 0 にすると、
        // 経路の上の «あるべき位置» と親子で決まる «実際の位置» がその節ぶんずれる。
        bone->transform.position =
            Vector3{ 0.0f, Max(m_links[i], 0.0f) * m_linkFade[i + 1], 0.0f };
        bone->transform.rotation = (world[i + 1].Inverse() * world[i]).Normalized();
    }

    // 折れ角。首と胴を分けて数える。
    //
    // WHY 地上の節だけ見るか: 口から口へ潜って渡る床下のリンクは、浅く保つぶん
    //     底で 12 度/関節 を少し超える。装甲の限界は «噛んで見える» ことの上限で、
    //     床下ではどこにも見えない ─ ここへ合わせてリンクを深くすると、渡っている
    //     最中に胴が全部床下へ入って蛇が消える。
    //
    // WHY 首の範囲が «狙いに使う関節数» か: 首を曲げているのは狙いそのもので、
    //     角が乗るのはちょうどその関節。境界を別に持つと、Joints を 4 から 6 へ
    //     変えた瞬間に «胴が噛んだ» と誤報し始める。
    const int neck = std::clamp(aimJoints, 1, serpent::kSegmentCount);
    float worstBody = 0.0f;
    float worstNeck = 0.0f;
    for (int i = 1; i <= serpent::kSegmentCount; ++i) {
        if (body && !body->IsAlive(i)) continue;
        if (i > 1 && body && !body->IsAlive(i - 1)) continue;
        if (!IsExposed(i) || !IsExposed(i - 1)) continue;
        const float angle = serpent::AngleDegrees(dir[i], dir[i - 1]);
        if (i <= neck) worstNeck = Max(worstNeck, angle);
        else           worstBody = Max(worstBody, angle);
    }
    debugBend     = worstBody;
    debugNeckBend = worstNeck;

    if (warnOnOverBend && !m_warnedBend &&
        (worstBody > bendLimitDegrees || worstNeck > neckBendLimitDegrees)) {
        m_warnedBend = true;
        const bool  neckBroke = worstNeck > neckBendLimitDegrees;
        const float worst     = neckBroke ? worstNeck : worstBody;
        const float limit     = neckBroke ? neckBendLimitDegrees : bendLimitDegrees;
        debug.LogWarning(
            std::string("SerpentSpineComponent: the ") + (neckBroke ? "neck" : "body") +
            " bends " + std::to_string(worst) + " deg per joint on route " + path->From() +
            " -> " + path->To() + ", over the " + std::to_string(limit) +
            " deg the armour rings allow." +
            (neckBroke ? " Lower Head Aim's Max / Joint, or spread it over more Joints."
                       : " Route across a wider gap (raise Min Chord) or lower Exposed "
                         "─ note that raising Exposed does NOT flatten the arch: with the "
                         "chord fixed, 9 m of arc bends 8.5 deg, 11.2 m bends 11.3 deg and "
                         "13 m bends 11.4 deg."));
    }
}

inline void SerpentSpineComponent::DriveRoll(Quaternion* world) const
{
    if (rollDegrees <= 0.01f) return;

    const float amplitude = ToRad(rollDegrees);
    const float k         = TWO_PI / Max(rollWavelength, 0.5f);

    for (int i = 0; i < serpent::kBoneCount; ++i) {
        // 床下では回さない。見えないうえに、縦坑の壁と装甲の角がすれ違う。
        const float local = m_surface01[i];
        if (local < 0.0f) continue;

        // 口の真上で 0 へ。sin(π·local) は弧の両端で 0、中ほどで 1 になる。
        // 端で回すと、開口の縁を装甲の角が舐める。
        const float gate = rollFadeAtMouth ? std::sin(PI * local) : 1.0f;
        if (gate <= 0.0f) continue;

        // 位相は «その骨の弧長»。時間で引くと尾から頭へ流れる ─ 横揺れ (undulation)
        // と同じ向きなので、2 つの波が 1 つのうねりとして読める。
        const float arc   = m_headArc - m_offsets[i];
        const float phase = arc * k - Time::time * rollSpeed;
        const float angle = amplitude * gate * std::sin(phase);

        // 骨のローカル +Y が «次の関節» を向く軸。world[i] を通した後の +Y まわりに
        // 回すので、向き (dir) は 1 度も動かない。
        const Vector3 axis = (world[i] * Vector3::UP).NormalizedOr(Vector3::UP);
        world[i] = (Quaternion::FromAxisAngle(axis, angle) * world[i]).Normalized();
    }
}

inline void SerpentSpineComponent::DriveAim(Vector3* dir, Quaternion* world)
{
    // 押されなかったフレームは «見るのをやめた» ということ。0 へ落として経路へ戻す。
    const float dt   = Max(Time::deltaTime, 0.0f);
    const float want = m_aimFresh ? m_aimWanted : 0.0f;
    m_aimFresh = false;
    m_aimWeight += (want - m_aimWeight) * Clamp01(aimResponse * dt);

    debugAim = m_aimWeight;
    if (m_aimWeight <= 0.001f) return;

    // 頭が床下に居るあいだは向けない。潜っている最中に振り返ると、
    // 開口の縁へ首を差し込むうえに «逃げている» が読めなくなる。
    if (!HeadIsExposed()) return;

    const Vector3 toTarget = (m_aimPoint - m_joints[0]);
    if (toTarget.LengthSq() < 0.04f) return;
    const Vector3 want3 = toTarget.NormalizedOr(dir[0]);

    const int   joints = std::clamp(aimJoints, 1, serpent::kSegmentCount);
    const float total  = serpent::AngleDegrees(dir[0], want3) * m_aimWeight;

    // 首の 1 関節目 (Head↔S01) から先に使い、余った角だけを胴の関節へこぼす。
    //
    // WHY 均等に割らないか: 均等だと胴の関節にも同じ角が乗る。弧そのものが
    //     11.3度/関節 ある内輪の経路では、そこへ 9 度足しただけで胴の装甲が噛む
    //     (実測 20.4度)。首は 40 度まで噛まないので、曲げてよい所から先に使う。
    //     胴へこぼす分は «上限 − 弧が既に使っている分» に頭打ちする。
    const auto* path      = Path();
    float       arcRadius = 0.0f;
    const float arcBend =
        (path && path->SurfaceLocal(m_headArc, nullptr, &arcRadius))
            ? SerpentPathComponent::BendPerJointDegrees(arcRadius, Max(m_links[1], 0.1f))
            : 0.0f;
    const float neckAllow = Max(aimMaxPerJoint, 0.0f);
    const float bodyAllow = BendBudgetDegrees(arcBend);

    const Vector3 axis = Vector3::Cross(dir[0], want3).NormalizedOr(Vector3::UP);

    // 根元から順に積む。子ほど «祖先ぶんの回転» を重ねて持つので、合計で狙いへ届く。
    float give[serpent::kBoneCount]{};
    float left = total;
    for (int i = 0; i < joints; ++i) {
        give[i] = Min(i == 0 ? neckAllow : bodyAllow, Max(left, 0.0f));
        left -= give[i];
    }

    Quaternion accum = Quaternion::Identity();
    for (int i = joints - 1; i >= 0; --i) {
        if (give[i] > 0.01f)
            accum = (Quaternion::FromAxisAngle(axis, ToRad(give[i])) * accum).Normalized();
        world[i] = (accum * world[i]).Normalized();
        dir[i]   = accum * dir[i];
    }
}

inline void SerpentSpineComponent::DriveHitboxActivity()
{
    auto* rig  = Rig();
    auto* body = Body();
    if (!rig) return;

    int exposed = 0;
    for (int i = 0; i < serpent::kBoneCount; ++i) {
        GameObject* hitbox = rig->SegmentHitbox(i);
        if (!hitbox) continue;

        const bool alive = i == 0 || !body || body->IsAlive(i);
        const bool live  = alive && IsExposed(i);
        if (hitbox->activeSelf() != live) hitbox->SetActive(live);
        if (live && i > 0) ++exposed;
    }
    debugExposed = exposed;
}

inline void SerpentSpineComponent::OnUpdate()
{
    EnsureBones();
    Solve();
    DriveHitboxActivity();
    debugHeadArc = m_headArc;

    if (!drawSpine) return;
    for (int i = 1; i < serpent::kBoneCount; ++i)
        debug.DrawLine(m_joints[i - 1], m_joints[i],
                       IsExposed(i) ? Vector4{ 0.2f, 1.0f, 0.5f, 1.0f }
                                    : Vector4{ 0.4f, 0.4f, 0.5f, 1.0f });
}

} // namespace sandbox
