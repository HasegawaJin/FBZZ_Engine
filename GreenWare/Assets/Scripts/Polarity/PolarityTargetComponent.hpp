/// @file    PolarityTargetComponent.hpp
/// @brief   極性を帯びられる対象。「極を持てるのは敵と撃破コアだけ」に該当するものへ付ける
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY プレイヤー側ではないのにここにあるか:
///   極性レーザーが照らす先がこれである。これが無いと「なぞっても何も起きない」ため、
///   Docs/presentation.md が最優先とした命中の手応えを一度も確認できない。
///   塗り進捗のプレビューも、発光を書いているここが受け持つ。
///
/// WHY 中和の «結果» を塗る前に見せないか:
///   旧仕様は逆極に触れた対象へ警告のバツ印を出していた。結果が塗る前に全部見えると
///   判断する必要が無くなり、唯一残っていた失敗要因が表示で潰れていた。
///   今は見せる代わりに罰を用意する (neutralizeLockSeconds)。
#pragma once

#include <Scripts/Data/PolarityTuning.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Game/VfxManagerComponent.hpp>
#include <Scripts/Utils/GlowMaterial.hpp>
#include <Scripts/Utils/LoopVoice.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <cmath>
#include <functional>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 持続時間テーブルのどれを使うか。質量ではなく役割で選ばせる。
// WHY 秒数を直接持たせないか: 各インスタンスに秒数を書けるようにすると、
//     同じ敵でも配置ごとに値がばらつき、PolarityTuning 側で制約②を検算できなくなる。
//     種別だけを選ばせ、値は共有アセットから引く。
// 残りがこの秒数を切ったら 1 度だけ予告音を鳴らす。
// WHY 割合ではなく秒か: 種別で持続時間が違うので割合にすると、Roller は 2 秒前・
//     Mite は 0.5 秒前になり、同じ音が別の意味になる。「間に合うか」の感覚は
//     残り秒数そのものなので、こちらを固定する。
//
// WHY 旧値 1.0 秒から下げたか: 最短の持続そのものが 2.5 秒まで詰まった。1.0 秒だと
//     帯電時間の 4 割が «予告中» になり、鳴らない方が珍しくなって合図として働かない。
inline constexpr float kExpiryWarnSeconds = 0.6f;

enum class PolarityClass : int {
    NormalSlime  = 0,
    ShooterSlime = 1,
    HeavySlime   = 2,
    Pillar       = 3,
};

class PolarityTargetComponent : public Script {
    FBZZ_SCRIPT(PolarityTargetComponent)

    // 発光色の駆動に MaterialInstance を使うため、スロットを持つ Material が要る。
    // 無いと極性が乗っても見た目が一切変わらず、12.4「残り時間の可視化は仕様」が崩れる。
    //
    // WHY REQUIRE ではなく OPTIONAL か: emissiveTargets で子オブジェクトを指した場合、
    //     Material は自分ではなくそちらに付く。マテリアル単位でメッシュを分けた敵
    //     (Mite / Serpent / Roller) では制御スクリプトがルート・発光メッシュが子、という
    //     構成が正しいのに、自分に必須と宣言すると Inspector が永久に不足を警告する。
    //     解決できなかった場合は OnStart が実行時エラーとして報告する。
    FBZZ_OPTIONAL_COMPONENT(MaterialComponent)

public:
    FBZZ_REQUIRED_ASSET(PolarityTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("持続時間と明滅の共有調整値。未割り当てでは動作を開始しない")

    // 表示名に 8 章の敵名を併記する。列挙子はスライム時代の名前のままだが、
    // 18.4 で敵 3 種が Mite / Serpent / Roller に確定しているので、Inspector で
    // 「Serpent に Shooter Slime を選ぶ」という読み替えを毎回させない。
    FBZZ_FIELD_ENUM(PolarityClass, polarityClass, PolarityClass::NormalSlime, "Class",
                    "Normal / Mite", "Shooter / Serpent", "Heavy / Roller", "Pillar / Core")
    FBZZ_TOOLTIP("7.2 の持続時間テーブルのどれを使うか")

    // 引力で動かない側 (7.3)。柱とヘビースライムが該当する。
    // 引力の実装は次フェーズだが、対象の性格はここで確定させておく。
    FBZZ_FIELD(bool, isAnchor, false, "Is Anchor")
    FBZZ_TOOLTIP("true なら引力で動かない。柱とヘビースライムに立てる")

    // 8 章「ボスには極性を付与できない。代わりに、ボスが数秒ごとに自分の極を切り替える」。
    //
    // WHY 盤面から外さず «塗れないだけ» にするか:
    //   ボスは引力の相手として盤面に居なければならない (逆極の雑魚を吸い寄せる側)。
    //   PolarityFieldComponent が候補を集める入口はこのスクリプトだけなので、外すと
    //   «ボスへ雑魚をぶつける» という唯一の攻略法そのものが成立しなくなる。
    //   居るけれど塗れない、という状態をここで表す。
    //
    // WHY 4 つの振る舞いを 1 つのフラグに束ねるか:
    //   「この極は自分で決める」と言った時点で、外から塗れないこと・時間で切れないこと・
    //   見た目を持ち主が描くことは同じ 1 つの決定から出てくる。別々のフラグにすると、
    //   片方だけ立てた «塗れないのに勝手に切れる» ボスが作れてしまう。
    FBZZ_FIELD(bool, selfDriven, false, "Self Driven")
    FBZZ_TOOLTIP("true なら銃で塗れず、時間でも切れない。極も発光も持ち主のスクリプトが決める")

    // 発光を書き込む Material スロット。SkinnedMeshRenderer は 1 GameObject で
    // モデル全体を描くため、光らせたいサブメッシュを番号で指す。
    FBZZ_FIELD_RANGE_INT(int, emissiveSlot, 0, "Emissive Slot", 0, 15)

    // 発光を書き込む GameObject。空なら自分自身の Material を使う (従来動作)。
    //
    // WHY 複数指定できるようにするか: 敵はマテリアル単位でメッシュを分けてあり、
    //     コア (M_E_Core_*) とリング (M_E_Ring_*) が別の GameObject になっている。
    //     8 章はこの 2 つが «同時に同じ色へ変わる» ことを極の読み取り記号としているので、
    //     どちらか一方しか光らせられないと、12.2 の «色が意味を持つ» が半分しか成立しない。
    FBZZ_REF_LIST_FIELD(GameObject, emissiveTargets, "Emissive Targets")
    FBZZ_TOOLTIP("極性色を書き込む発光メッシュ。空なら自分自身。コアとリングを両方指定する")

    // 帯電しているあいだ、シルエットを放電する輪郭で縁取る。
    //
    // WHY 発光 (emissive) と別に要るか:
    //   12.4 の明滅は «その敵をちゃんと見ている» ことが前提で、コンボの最中に
    //   プレイヤーが見ているのは次の 1 体か照準。輪郭は形の外側に出るので、
    //   視界の端でも «何体が帯電しているか» が数えられる。色は極そのものを使う。
    FBZZ_GROUP("Outline")
    FBZZ_FIELD(bool, outlineEnabled, true, "Outline")
    FBZZ_TOOLTIP("帯電中に放電する輪郭を掛ける。太さと明滅は ScreenEffectManager が持つ")
    FBZZ_FIELD_RANGE(float, outlineWidth, 1.0f, "Outline Width", 0.1f, 1.0f)
    FBZZ_TOOLTIP("ScreenEffectManager の Width に対する比。大きい相手ほど細くすると "
                 "«近さ» が輪郭の太さで読める")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")

    // ── 問い合わせ ──────────────────────────────────────────────────────
    FBZZ_FIELD(bool, acceptsPaint, false, "Accepts Paint")
    FBZZ_TOOLTIP("Self Driven のまま塗りを受け付ける。ボスを «逆極を撃ち込む的» に "
                 "するときだけ入れる。持ち主側も追従する構えになっていること")

    [[nodiscard]] Polarity Current() const { return m_polarity; }
    [[nodiscard]] bool  IsCharged() const { return m_polarity != Polarity::None; }
    /// 今の極が乗った時刻 [Time::time]。集束点 (最後に塗った 1 体) を選ぶのに使う。
    [[nodiscard]] float ChargedAt() const { return m_chargedAt; }

    // ── 反発 (同極) ──────────────────────────────────────────────────────
    /// 弾かれ方を持ち主のスクリプトが引き受ける差し込み口。
    ///
    /// WHY 関数で渡すか: 反発を受けて «どう動くか» は対象ごとに違う。Mite は飛び、
    ///     Roller は向きを変えて転がり出す (Docs/enemies.md)。盤面 (PolarityFieldComponent)
    ///     が敵の種類を知ろうとすると、盤面から敵のスクリプトへ include が伸びて輪になる。
    ///     «誰が弾かれたか» を決めるのは盤面、«どう動くか» を決めるのは本人、で切る。
    ///     未設定なら盤面が PolarityBodyComponent へ速度を与える既定の弾け方になる。
    std::function<void(const Vector3& direction, float speed)> onRepulse;

    /// 今フレーム弾いてよいか。連続で弾くと押し合いが毎フレーム反転して画面が痙攣する。
    [[nodiscard]] bool CanBeRepulsed() const { return m_repulseLock <= 0.0f; }
    /// 弾いた側が呼ぶ。次に弾けるまでの間隔を張る。
    void NotifyRepulsed() { m_repulseLock = Max(tuning->repulseCooldown, 0.0f); }

    // ── リンクの一時停止 ──────────────────────────────────────────────────
    /// 今は引力・集束の相手として選ばれない状態か。
    ///
    /// WHY 要るか: 転がっている Roller は «的» として使えない (Docs/enemies.md)。
    ///     押した瞬間に的が的でなくなる、というのが Roller 唯一の «順番の強制» で、
    ///     盤面がそれを知る手段がここしかない。isAnchor は «動かない側» の宣言で、
    ///     転がっていても動かない側であることは変わらないため流用できない。
    [[nodiscard]] bool IsLinkSuspended() const { return m_linkSuspended; }
    /// 本人が毎フレーム申告する。
    void SetLinkSuspended(bool suspended) { m_linkSuspended = suspended; }

    // ── 中和の罰 ────────────────────────────────────────────────────────
    /// 中和された直後で、どの極も受け付けない状態か。
    [[nodiscard]] bool IsPaintLocked() const { return m_paintLock > 0.0f; }
    /// 罰の残り [秒]。HUD と照準表示が «この 1 体は今使えない» を出すために読む。
    [[nodiscard]] float PaintLockRemaining() const { return Max(m_paintLock, 0.0f); }
    [[nodiscard]] float RemainingSeconds() const { return m_remaining; }
    // 極性照射を浴びた直後の短い硬直。敵 AI はこの間だけ移動・攻撃を止める。
    [[nodiscard]] bool IsHitReacting() const { return m_hitReactRemaining > 0.0f; }
    // 1 = 付与直後 / 0 = 切れる直前。明滅速度と濃さの両方がこれで決まる。
    [[nodiscard]] float RemainingNormalized() const;
    // 7.2 の種別ごとの基準持続時間。
    [[nodiscard]] float BaseDuration() const;

    // 今 incoming を塗り切ると中和になるか。
    //
    // WHY 表示に使わないのに残すか: 中和を «道具» として狙う側 (リンクを切る) が
    //     結果を知りたい場面はある。プレイヤーへ塗る前に見せる用途では使わない。
    [[nodiscard]] bool WouldNeutralize(Polarity incoming) const;

    // ── なぞり塗り (6.1 / 6.2) ──────────────────────────────────────────
    // ビームが触れている間、毎フレーム呼ぶ。接触が Paint Seconds に達したフレームだけ
    // 極性を適用して true を返し、outResult に何が起きたかを入れる。
    //
    // WHY 進捗を対象側で持つか: 塗り進捗のプレビューは「この敵がどこまで塗れたか」の
    //     表示で、発光を書いているのはこのスクリプトである。照射側で持つと、
    //     見た目を出すために進捗を毎フレーム押し戻すことになり、2 本の銃と
    //     対象の対応表を照射側が抱えることになる。塗られる側が自分の進捗を持つ。
    bool Paint(Polarity incoming, float dt, PolarityResult& outResult);

    // 塗り進捗のプレビュー表示用。進捗 0 のときは塗られていない。
    [[nodiscard]] float    PaintProgress() const { return m_paintProgress; }
    [[nodiscard]] Polarity PaintPolarity() const { return m_paintPolarity; }

    // タップの一瞬の点付与 (6.2) と、7 章のルール 3 行の適用口。
    // 戻り値の change を見て、呼び出し側が延長 / 中和のフィードバックを出し分ける。
    PolarityResult Apply(Polarity incoming);

    // 中和以外で強制的に落とす場合 (衝突で倒れた・Wave リセット等)。
    void ClearPolarity();

    /// selfDriven の対象の極を、持ち主のスクリプトが直接決める。
    /// WHY 発光を書かないか: selfDriven は «見た目も持ち主が描く» を含む。ここで描くと、
    ///     切替予兆の明滅 (8 章) と残り時間の明滅 (12.4) が同じ材質へ二重に書き込まれる。
    void SetPolarity(Polarity polarity);

    void OnStart()  override;
    void OnUpdate() override;
    // 無効化された対象の帯電音が盤面に残らないようにする。
    void OnDisable() override { m_chargedVoice.Stop(*this); }

private:
    void ApplyVisual();
    /// 帯電している «あいだ» ずっと鳴る音を、極に追従させる。
    void DriveChargedVoice();
    /// 帯電している «あいだ» ずっと、自分の形を輪郭マスクへ申告する。
    void DriveOutline();
    /// 発光を書き込める Material が 1 つでも解決できるか。
    [[nodiscard]] bool HasEmissiveTarget() const;
    /// 極が «変わった» ときだけ単発 VFX を鳴らす。延長では何も出さない。
    void PlayChangeVfx(const PolarityResult& result);
    // 塗り進捗を捨てる。塗り切った / 中断した / 極が変わった、のいずれでも呼ぶ。
    void ResetPaint();
    [[nodiscard]] float PaintSeconds() const;

    Polarity m_polarity  = Polarity::None;
    /// 今の極が乗った時刻。無極になったら 0 へ戻す。
    float    m_chargedAt = 0.0f;
    float    m_remaining = 0.0f;
    // 現在の帯電が始まったときの基準持続時間。延長の上限計算に使う。
    float    m_chargeBase = 0.0f;
    float    m_hitReactRemaining = 0.0f;

    // なぞり塗りの進捗 0..1 と、それを塗っている極。
    Polarity m_paintPolarity = Polarity::None;
    float    m_paintProgress = 0.0f;
    // 中和の罰。0 より大きい間はどの極も乗らない。
    float    m_paintLock   = 0.0f;
    // 反発の間隔。0 より大きい間は弾かれない。
    float    m_repulseLock = 0.0f;
    // 今フレーム、引力・集束の相手として選ばれないか。
    bool     m_linkSuspended = false;
    // 切れる予告をこの帯電で 1 度だけ鳴らすためのラッチ。
    bool     m_warnedExpiry = false;
    // 今フレーム照射に触れられたか。OnUpdate が読んだ直後に倒す。
    //
    // WHY フラグで持つか: 照射側と対象側はどちらも Script フェーズで回り、順序は
    //     決まっていない。「触られていなければ戻す」を触られた事実で判定すれば、
    //     どちらが先に回っても 1 フレームずれるだけで結果は変わらない。
    bool     m_paintedThisFrame = false;

    // WHY 主 voice で鳴らさないか: 帯電音は塗られてから切れるまで鳴り続ける。
    //     同じ口から出すと、その音量とピッチが中和音・被弾音・撃破音にも掛かる
    //     (LoopVoice.hpp)。鳴り続けているものは別の口から出す。
    se::LoopVoice m_chargedVoice;
};

FBZZ_REFLECT(PolarityTargetComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

inline float PolarityTargetComponent::BaseDuration() const
{
    switch (polarityClass) {
    case PolarityClass::ShooterSlime: return tuning->durationShooter;
    case PolarityClass::HeavySlime:   return tuning->durationHeavy;
    case PolarityClass::Pillar:       return tuning->durationPillar;
    case PolarityClass::NormalSlime:  break;
    }
    return tuning->durationNormal;
}

inline float PolarityTargetComponent::RemainingNormalized() const
{
    if (m_chargeBase <= 0.0f) return 0.0f;
    return Clamp01(m_remaining / m_chargeBase);
}

inline bool PolarityTargetComponent::WouldNeutralize(Polarity incoming) const
{
    // 塗れない相手には何も起きない。
    if (selfDriven) return false;
    return ResolvePolarity(m_polarity, incoming).change == PolarityChange::Neutralized;
}

inline float PolarityTargetComponent::PaintSeconds() const
{
    return Max(tuning->paintSeconds, 0.0f);
}

inline void PolarityTargetComponent::ResetPaint()
{
    m_paintPolarity    = Polarity::None;
    m_paintProgress    = 0.0f;
    m_paintedThisFrame = false;
}

inline bool PolarityTargetComponent::Paint(Polarity incoming, float dt,
                                           PolarityResult& outResult)
{
    if (selfDriven) return false;
    if (incoming == Polarity::None || dt <= 0.0f) return false;
    // 中和の罰。塗り «進捗» ごと拒む。進捗だけ溜めさせると、罰が明けた瞬間に
    // 押していただけで極が乗り、一手ぶん無駄になったという事実が消える。
    if (m_paintLock > 0.0f) return false;

    // 逆の極でなぞられ始めたら進捗は最初からやり直す。片方の銃で半分塗った途中に
    // もう片方が触れたとき、進捗を共有すると「触れていない方の極が乗る」が起きる。
    if (m_paintPolarity != incoming) {
        m_paintPolarity = incoming;
        m_paintProgress = 0.0f;
    }
    m_paintedThisFrame = true;

    const float seconds = PaintSeconds();
    // 塗り時間 0 は「触れた瞬間に付く」。割り算を避けるためだけの分岐ではなく、
    // 18.0 の数値検討でここを 0 にして試す場面が実際にある。
    m_paintProgress = seconds > 0.0f ? Clamp01(m_paintProgress + dt / seconds) : 1.0f;
    if (m_paintProgress < 1.0f) {
        ApplyVisual();
        return false;
    }

    outResult = Apply(incoming);
    return true;
}

inline PolarityResult PolarityTargetComponent::Apply(Polarity incoming)
{
    // selfDriven は «極を決めるのは持ち主» という宣言で、既定では塗りを断る
    // (8 章「ボスには極性を付与できない」)。入口 1 箇所で断り、何も起きなかったことを
    // Extended (無変化) として返す。
    //
    // WHY 例外を作れるようにするか: ボスを «逆極を撃ち込む的» にする遊びでは、
    //     プレイヤーが極を乗せられないと集束点が盤面に存在しなくなる。ただし
    //     発光とフェーズは持ち主 (BossPolarityCoreComponent) が描き続けるので、
    //     selfDriven そのものは降ろせない。«決めるのは持ち主 / 乗せるのは外» を
    //     分けられるよう、受け入れだけを別のスイッチにする。
    if (selfDriven && !acceptsPaint) return { m_polarity, PolarityChange::Extended };
    // 中和の罰の最中。タップの点付与もここを通るので、両方の入口で同じだけ待たされる。
    if (m_paintLock > 0.0f) return { m_polarity, PolarityChange::Extended };

    const PolarityResult result = ResolvePolarity(m_polarity, incoming);
    m_hitReactRemaining = tuning->hitReactSeconds;
    // 塗り切った / タップで乗せた時点でプレビューの役目は終わる。残すと
    // 適用後の色の上に「これから塗る色」が重なって、どちらが結果か読めなくなる。
    ResetPaint();

    // 残り時間が動くので、予告はこの帯電に対して 1 からやり直す。
    m_warnedExpiry = false;

    switch (result.change) {
    case PolarityChange::Applied:
        m_polarity   = result.polarity;
        m_chargeBase = BaseDuration();
        m_remaining  = m_chargeBase;
        // 7.9 の集束点を決めるのに «どれが最後に塗られたか» が要る。延長 (Extended) では
        // 更新しない ─ 既に武装している 1 体を撃ち直しただけで、盤面に置いた極の並びは
        // 変わっていないため。ここを延長でも動かすと、溜めた列を撫でただけで集束点が
        // 移り、仕込みが崩れる。
        m_chargedAt = Time::time;
        // WHY 銃側の確定音と重ねるか: 銃の音は「塗れた」という手元の応答で、
        //     こちらは「この個体が帯びた」という盤面の出来事。前者は 2D、後者は
        //     その敵の位置から鳴るので、どこに乗ったかが定位で分かる。
        se::Play(audio, se::kPolarityInfect);
        break;

    case PolarityChange::Extended: {
        // 残り時間へ加算する。上限が無いと 1 体を撃ち続けるだけで永久に帯電でき、
        // 3.3 の「残り何秒かを把握し続ける」という思考そのものが消える。
        const float base    = BaseDuration();
        const float ratio   = tuning->extendRatio;
        const float capMul  = tuning->extendCapRatio;
        m_remaining = Min(m_remaining + base * ratio, base * capMul);
        // 上限まで伸びた状態を 1.0 として扱えるよう、基準も伸ばす。
        m_chargeBase = Max(m_chargeBase, m_remaining);
        break;
    }

    case PolarityChange::Neutralized:
        m_polarity   = Polarity::None;
        m_remaining  = 0.0f;
        m_chargeBase = 0.0f;
        // 罰を張る。中和は «リンクを切る» 道具でもあるので、狙って撃った側にも
        // 同じだけ待ちが掛かる。道具として強すぎないための代償でもある。
        m_paintLock  = Max(tuning->neutralizeLockSeconds, 0.0f);
        break;
    }

    PlayChangeVfx(result);
    ApplyVisual();
    return result;
}

// 12.1 の「着弾点から広がる帯電エフェクト」と、12.4 が仕様として要求する中和の反応。
//
// WHY 延長では何も出さないか:
//   延長は既に帯びている極の残り時間が伸びただけで、盤面の構成は変わらない。付与と
//   同じ絵を返すと、なぞりの途中で «また 1 体増えた» と読み違え、集束したときの
//   コンボ数が数えていたものと合わなくなる。延長の手応えは 12.4 の明滅と
//   銃側の Paint_Extend が既に持っている。
inline void PolarityTargetComponent::PlayChangeVfx(const PolarityResult& result)
{
    auto* vfx = VfxManagerComponent::Instance();
    GameObject* self = scene.Self();
    if (!vfx || !self) return;

    switch (result.change) {
    case PolarityChange::Applied:     vfx->PlayCharge(*self, result.polarity); break;
    case PolarityChange::Neutralized: vfx->PlayNeutralize(*self);              break;
    case PolarityChange::Extended:    break;
    }
}

inline void PolarityTargetComponent::ClearPolarity()
{
    m_polarity   = Polarity::None;
    m_chargedAt  = 0.0f;
    m_remaining  = 0.0f;
    m_chargeBase = 0.0f;
    // WHY 罰まで畳むか: ここは «強制的に落とす» 入口 (衝突・Wave リセット・枠の使い回し)
    //     で、中和の結果として無極になる経路 (Apply) はここを通らない。
    //     残したままにすると、使い回された撃破コアの枠が «前の中和の罰» を引き継いで
    //     極を受け付けず、生まれた瞬間に消える。
    m_paintLock  = 0.0f;
    ApplyVisual();
}

inline void PolarityTargetComponent::SetPolarity(Polarity polarity)
{
    m_polarity = polarity;
    // 残り時間は持たない。selfDriven の極は «切り替わる» のであって «切れる» ことがなく、
    // 12.4 の残り時間の明滅もここでは意味を持たない。
    m_remaining  = 0.0f;
    m_chargeBase = 0.0f;
}

inline void PolarityTargetComponent::OnStart()
{
    if (!tuning) {
        debug.LogError("PolarityTargetComponent requires PolarityTuning.fzdata.");
        enabled = false;
        return;
    }
    // 開始時は必ず無極。前回 Play の状態が見た目に残らないようにする。
    m_hitReactRemaining = 0.0f;
    m_warnedExpiry      = false;
    m_paintLock         = 0.0f;
    m_repulseLock       = 0.0f;
    ResetPaint();
    ClearPolarity();

    // 発光の書き込み先が 1 つも解決できないと、極が乗っても盤面の色は変わらない。
    // 12.4 が可視化を «演出ではなく仕様» と書いている以上、黙って無反応にはしない。
    // selfDriven は発光を持ち主が描くので、こちらに書き込み先が無いのが正しい構成。
    if (!selfDriven && !HasEmissiveTarget()) {
        debug.LogError("PolarityTargetComponent has no material to drive. Add a "
                       "MaterialComponent here, or assign Emissive Targets.");
    }

    // 極が切れる音は「その敵から」聞こえないと、盤面のどれが切れたのか分からない。
    se::EnsureSource(scene, "SE", 1.0f);

    // 持ち主ごとに一意でないと、盤面の対象どうしが同じ子音源を奪い合う。
    m_chargedVoice.SetKey("Charged");
    m_chargedVoice.Stop(*this);
}

inline void PolarityTargetComponent::DriveChargedVoice()
{
    // WHY 帯電中ずっと鳴らすか: 12.4 は残り時間の可視化を «演出ではなく仕様» と
    //     書いていて、明滅がその答えになっている。ただし明滅は見ていないと気付けず、
    //     コンボの最中プレイヤーは次の 1 体を見ている。«今いくつ帯電しているか» と
    //     «どちらの極か» が、盤面を見ずに極ごとの音色で分かるようにする。
    if (m_polarity == Polarity::None) {
        m_chargedVoice.Stop(*this);
        return;
    }
    // 変奏は選ばない。ループは掛け直すたびに別のクリップへ変わってはいけない。
    m_chargedVoice.Update(*this, se::PolarityChargedLoop(m_polarity).First(),
                          tuning->chargedLoopVolume);
}

// WHY 毎フレーム申告するか (乗った瞬間に 1 度ではなく):
//   輪郭の申告は «そのフレームだけ» 有効という約束になっている (ScriptObjectMaskProxy)。
//   撃破・Play 停止・スクリプトの無効化で消し忘れが残らないための約束なので、
//   出し続けたい側が言い続ける。
inline void PolarityTargetComponent::DriveOutline()
{
    if (!outlineEnabled || m_polarity == Polarity::None) return;

    // マスクの意味は読む側 (PolarityOutline.hlsl) との取り決め: RGB = 極の色 / A = 太さ。
    objectMask.Set(PolarityColor(m_polarity), Clamp01(outlineWidth));

    // 縁取り方を持つポストプロセスはこちらでは載せられない (customEffects は
    // ScreenEffectManagerComponent が丸ごと差し替える器のため)。盤面に帯電が
    // 1 つでもあることを知っているのはこちらなので、点けに行くのもこちらから。
    if (auto* screen = ScreenEffectManagerComponent::Instance()) screen->KeepOutline();
}

inline bool PolarityTargetComponent::HasEmissiveTarget() const
{
    const uint32_t slot = static_cast<uint32_t>(emissiveSlot);
    if (emissiveTargets.empty()) return material.Instance(slot).IsValid();

    for (const auto& target : emissiveTargets)
        if (target.IsAssigned() && material.Instance(target.ref, slot).IsValid())
            return true;
    return false;
}

inline void PolarityTargetComponent::OnUpdate()
{
    const float dt = Time::deltaTime;
    m_hitReactRemaining = Max(0.0f, m_hitReactRemaining - dt);
    // 罰と反発の間隔は selfDriven でも進める。ボスも弾かれる側には回るし、
    // «塗りを受け付ける» ボスは中和もされる。
    const bool wasPaintLocked = m_paintLock > 0.0f;
    m_paintLock   = Max(0.0f, m_paintLock   - dt);
    m_repulseLock = Max(0.0f, m_repulseLock - dt);
    // 明けた瞬間だけ描き直す。罰の «終わり» が絵に出ないと、いつ撃ち直せるのか
    // 分からないまま押し続けることになる。
    const bool paintLockJustEnded = wasPaintLocked && m_paintLock <= 0.0f;

    // 塗られることも切れることもない対象は、ここで見るものが何も無い。
    // 発光は持ち主が毎フレーム描いているので、こちらが上書きしてはいけない。
    if (selfDriven) {
        // ただし «塗りを受け付ける» selfDriven (集束点にするボス) は別。残り時間を
        // 進めないと、一度乗った極が永久に残る。
        //
        // WHY それが致命的か: ボスが帯電しっぱなしだと、次に雑魚を塗った瞬間その 1 体が
        //     単独でボスへ飛んでいく。«何体か並べてから撃ち込む» が原理的に組めなくなり、
        //     仕込みという行為そのものが消える。切れるからこそ «窓» ができる。
        //
        // 発光は持ち主が描くので、ここでは ApplyVisual を呼ばない。
        if (acceptsPaint && m_polarity != Polarity::None) {
            m_remaining -= dt;
            if (m_remaining <= 0.0f) {
                se::Play(audio, se::kPolarityExpire);
                ClearPolarity();
                m_warnedExpiry = false;
            } else if (!m_warnedExpiry && m_remaining <= kExpiryWarnSeconds) {
                m_warnedExpiry = true;
                se::Play(audio, se::kPolarityExpireWarn);
            }
        }
        debugRemaining = Max(m_remaining, 0.0f);
        // 発光は持ち主が描くが、輪郭の «形» は誰が描いても同じものになる。
        // ボスだけ輪郭が出ないと、極を持っているのに盤面の記号から外れる。
        DriveOutline();
        return;
    }

    // 6.2「素早く振ると塗り残す」。触れていないフレームは進捗を戻す。
    bool previewFaded = false;
    if (m_paintedThisFrame) {
        m_paintedThisFrame = false;
    } else if (m_paintProgress > 0.0f) {
        const float seconds = PaintSeconds();
        const float decay = seconds > 0.0f ? dt / seconds * Max(tuning->paintDecayScale, 0.0f)
                                           : 1.0f;
        m_paintProgress = Max(0.0f, m_paintProgress - decay);
        if (m_paintProgress <= 0.0f) {
            m_paintPolarity = Polarity::None;
            previewFaded    = true;
        }
    }

    if (m_polarity == Polarity::None) {
        debugRemaining = 0.0f;
        DriveChargedVoice();
        // 無極でもプレビューは出る (塗り進捗のアウトラインが満ちていく)。
        // ただし塗られてもいない対象まで毎フレーム MaterialInstance を叩くと、
        // 盤面の全対象ぶんの無駄書きになる。動いたときだけ書く。
        if (m_paintProgress > 0.0f || previewFaded || m_paintLock > 0.0f ||
            paintLockJustEnded)
            ApplyVisual();
        return;
    }

    DriveChargedVoice();
    DriveOutline();

    m_remaining -= dt;
    if (m_remaining <= 0.0f) {
        // 12.4 の「残り時間の可視化は仕様」に耳の側から対応する。明滅は見ていないと
        // 気付けないが、コンボの最中はプレイヤーは次の 1 体を見ている。
        se::Play(audio, se::kPolarityExpire);
        ClearPolarity();
        // 切れた «その瞬間» に土台を畳む。次のフレームまで残すと、切れる音と
        // 帯電音が 1 フレーム重なって «まだ生きている» と読める。
        m_chargedVoice.Stop(*this);
        debugRemaining = 0.0f;
        m_warnedExpiry = false;
        return;
    }

    // 切れる直前の予告。ここを過ぎたら次の 1 体を塗るのは間に合わない、という
    // 境目を音で 1 度だけ知らせる。毎フレーム鳴らすと明滅と同じで背景になる。
    if (!m_warnedExpiry && m_remaining <= kExpiryWarnSeconds) {
        m_warnedExpiry = true;
        se::Play(audio, se::kPolarityExpireWarn);
    }

    debugRemaining = m_remaining;
    ApplyVisual();
}

inline void PolarityTargetComponent::ApplyVisual()
{
    // 12.4 は残り時間の可視化を「演出ではなく仕様」と書いている。
    //   - 残りが減るほど明滅が速くなる
    //   - 切れる直前に色が薄くなる
    const float remaining = RemainingNormalized();
    const float hzMin  = tuning->blinkHzMin;
    const float hzMax  = tuning->blinkHzMax;
    const float depth  = tuning->blinkDepth;

    // remaining 1 → hzMin / remaining 0 → hzMax
    const float hz    = Lerp(hzMax, hzMin, remaining);
    const float phase = std::sin(Time::time * hz * TWO_PI) * 0.5f + 0.5f;
    const float pulse = Lerp(1.0f - Clamp01(depth), 1.0f, phase);

    Vector4 color = PolarityColor(m_polarity);
    float   scale = m_polarity == Polarity::None
        ? 0.0f
        : kEmissiveBase * FadeFromRemaining(remaining) * pulse;

    // 塗っている «最中» のアウトラインが満ちていく表示。
    //
    // WHY 中和になる相手も同じ絵にするか: 結果を塗る前に見せる警告は廃止した。
    //     逆極を塗っているときも «これから乗る極の色が満ちていく» ように見え、
    //     満ちた瞬間に消える。撃つ前に分かるのは配置だけで、結果は撃ってから返る。
    if (m_paintProgress > 0.0f && m_paintPolarity != Polarity::None) {
        const float fill = Clamp01(m_paintProgress);
        const Vector4 target = PolarityColor(m_paintPolarity);
        color = color + (target - color) * fill;
        scale = Max(scale, kEmissiveBase * fill);
    }

    // 中和の罰の最中。«今この 1 体は撃っても無駄» を、極でも警告でもない
    // 消えかけの無彩色で出す。ここを派手にすると罰が «出来事» に見えて、
    // 失敗したのに何かを得たように読めてしまう。
    if (m_paintLock > 0.0f) {
        const float stutter = std::sin(Time::time * 9.0f * TWO_PI) * 0.5f + 0.5f;
        color = kColorNeutral;
        scale = kEmissiveBase * Lerp(0.05f, 0.28f, stutter);
    }

    const uint32_t slot = static_cast<uint32_t>(emissiveSlot);
    const auto write = [&](const MaterialInstance& instance) {
        if (!instance.IsValid()) return;
        instance.SetVector3(kEmissiveColorId, { color.x, color.y, color.z });
        instance.SetFloat(kEmissiveScaleId, scale);
    };

    if (emissiveTargets.empty()) {
        write(material.Instance(slot));
        return;
    }
    for (const auto& target : emissiveTargets) {
        // 未アサインの枠は Instance が自分自身へ落ちる。空欄を 1 つ残しただけで
        // ルート (発光しないメッシュ) が対象に混ざるのを防ぐ。
        if (!target.IsAssigned()) continue;
        write(material.Instance(target.ref, slot));
    }
}

} // namespace sandbox
