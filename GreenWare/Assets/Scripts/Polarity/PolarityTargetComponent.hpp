/// @file PolarityTargetComponent.hpp
/// @brief 極性を帯びられる対象。9 章の「極を持てるのは敵と撃破コアだけ」に該当するものへ付ける
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY プレイヤー側ではないのにここにあるか:
///   極性レーザーが照らす先がこれである。これが無いと「なぞっても何も起きない」ため、
///   企画書 12.1 が最優先とした命中の手応えを一度も確認できない。
///   6.5 のプレビュー (塗り進捗・中和の警告) も、発光を書いているここが受け持つ。
///
/// WHY プレイヤーには付けないか:
///   5 章「プレイヤー自身は極性を持たない」。引力は敵同士・敵と障害物の間でのみ発生する。
#pragma once

#include <Scripts/Data/PolarityTuning.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Utils/PolarityTypes.hpp>
#include <cmath>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

// 7.2 の持続時間テーブルのどれを使うか。質量ではなく役割で選ばせる。
// WHY 秒数を直接持たせないか: 各インスタンスに秒数を書けるようにすると、
//     同じノーマルスライムでも配置ごとに値がばらつき、7.7 の制約を
//     PolarityTuning 側で検算できなくなる。種別だけを選ばせ、値は共有アセットから引く。
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
    FBZZ_REQUIRE_COMPONENT(MaterialComponent)

public:
    FBZZ_REQUIRED_ASSET(PolarityTuning, tuning, "Tuning")
    FBZZ_TOOLTIP("持続時間と明滅の共有調整値。未割り当てでは動作を開始しない")

    FBZZ_FIELD_ENUM(PolarityClass, polarityClass, PolarityClass::NormalSlime, "Class",
                    "Normal Slime", "Shooter Slime", "Heavy Slime", "Pillar")
    FBZZ_TOOLTIP("7.2 の持続時間テーブルのどれを使うか")

    // 引力で動かない側 (7.3)。柱とヘビースライムが該当する。
    // 引力の実装は次フェーズだが、対象の性格はここで確定させておく。
    FBZZ_FIELD(bool, isAnchor, false, "Is Anchor")
    FBZZ_TOOLTIP("true なら引力で動かない。柱とヘビースライムに立てる")

    // 発光を書き込む Material スロット。SkinnedMeshRenderer は 1 GameObject で
    // モデル全体を描くため、光らせたいサブメッシュを番号で指す。
    FBZZ_FIELD_RANGE_INT(int, emissiveSlot, 0, "Emissive Slot", 0, 15)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")

    // ── 問い合わせ ──────────────────────────────────────────────────────
    [[nodiscard]] Polarity Current() const { return m_polarity; }
    [[nodiscard]] bool  IsCharged() const { return m_polarity != Polarity::None; }
    [[nodiscard]] float RemainingSeconds() const { return m_remaining; }
    // 極性照射を浴びた直後の短い硬直。敵 AI はこの間だけ移動・攻撃を止める。
    [[nodiscard]] bool IsHitReacting() const { return m_hitReactRemaining > 0.0f; }
    // 1 = 付与直後 / 0 = 切れる直前。明滅速度と濃さの両方がこれで決まる。
    [[nodiscard]] float RemainingNormalized() const;
    // 7.2 の種別ごとの基準持続時間。
    [[nodiscard]] float BaseDuration() const;

    // 今 incoming を塗り切ると中和になるか。6.5 が「塗る前に見える」ことを
    // 必須にしているため、照射側が触れた瞬間に警告を出せるようここで判定する。
    [[nodiscard]] bool WouldNeutralize(Polarity incoming) const;

    // ── なぞり塗り (6.1 / 6.2) ──────────────────────────────────────────
    // ビームが触れている間、毎フレーム呼ぶ。接触が Paint Seconds に達したフレームだけ
    // 極性を適用して true を返し、outResult に何が起きたかを入れる。
    //
    // WHY 進捗を対象側で持つか: 6.5 のプレビューは「この敵がどこまで塗れたか」の
    //     表示で、発光を書いているのはこのスクリプトである。照射側で持つと、
    //     見た目を出すために進捗を毎フレーム押し戻すことになり、2 本の銃と
    //     対象の対応表を照射側が抱えることになる。塗られる側が自分の進捗を持つ。
    bool Paint(Polarity incoming, float dt, PolarityResult& outResult);

    // 6.5 のプレビュー表示用。進捗 0 のときは塗られていない。
    [[nodiscard]] float    PaintProgress() const { return m_paintProgress; }
    [[nodiscard]] Polarity PaintPolarity() const { return m_paintPolarity; }

    // タップの一瞬の点付与 (6.2) と、7 章のルール 3 行の適用口。
    // 戻り値の change を見て、呼び出し側が延長 / 中和のフィードバックを出し分ける。
    PolarityResult Apply(Polarity incoming);

    // 中和以外で強制的に落とす場合 (衝突で倒れた・Wave リセット等)。
    void ClearPolarity();

    void OnStart()  override;
    void OnUpdate() override;

private:
    void ApplyVisual();
    // 塗り進捗を捨てる。塗り切った / 中断した / 極が変わった、のいずれでも呼ぶ。
    void ResetPaint();
    [[nodiscard]] float PaintSeconds() const;

    Polarity m_polarity  = Polarity::None;
    float    m_remaining = 0.0f;
    // 現在の帯電が始まったときの基準持続時間。延長の上限計算に使う。
    float    m_chargeBase = 0.0f;
    float    m_hitReactRemaining = 0.0f;

    // なぞり塗りの進捗 0..1 と、それを塗っている極。
    Polarity m_paintPolarity = Polarity::None;
    float    m_paintProgress = 0.0f;
    // 今フレーム照射に触れられたか。OnUpdate が読んだ直後に倒す。
    //
    // WHY フラグで持つか: 照射側と対象側はどちらも Script フェーズで回り、順序は
    //     決まっていない。「触られていなければ戻す」を触られた事実で判定すれば、
    //     どちらが先に回っても 1 フレームずれるだけで結果は変わらない。
    bool     m_paintedThisFrame = false;
};

FBZZ_REFLECT(PolarityTargetComponent)

// ── 実装 (inline) ─────────────────────────────────────────────────────────────

// Material プロパティ名。AssetBrowser が生成する .mat テンプレートの
// [params] セクションのキーと一致させる。
// MaterialInstance は HLSL リフレクション上の変数名を検証するため camelCase を使う。
// .mat 内の別名ではなくシェーダー変数へ合わせないと、警告だけ出て発光が更新されない。
inline constexpr MaterialPropertyId kEmissiveColorId{ "emissiveColor" };
inline constexpr MaterialPropertyId kEmissiveScaleId{ "emissiveScale" };

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
    if (incoming == Polarity::None || dt <= 0.0f) return false;

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
    const PolarityResult result = ResolvePolarity(m_polarity, incoming);
    m_hitReactRemaining = tuning->hitReactSeconds;
    // 塗り切った / タップで乗せた時点でプレビューの役目は終わる。残すと
    // 適用後の色の上に「これから塗る色」が重なって、どちらが結果か読めなくなる。
    ResetPaint();

    switch (result.change) {
    case PolarityChange::Applied:
        m_polarity   = result.polarity;
        m_chargeBase = BaseDuration();
        m_remaining  = m_chargeBase;
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
        break;
    }

    ApplyVisual();
    return result;
}

inline void PolarityTargetComponent::ClearPolarity()
{
    m_polarity   = Polarity::None;
    m_remaining  = 0.0f;
    m_chargeBase = 0.0f;
    ApplyVisual();
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
    ResetPaint();
    ClearPolarity();
}

inline void PolarityTargetComponent::OnUpdate()
{
    const float dt = Time::deltaTime;
    m_hitReactRemaining = Max(0.0f, m_hitReactRemaining - dt);

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
        // 無極でもプレビューは出る (6.5 の「アウトラインが満ちていく」)。
        // ただし塗られてもいない対象まで毎フレーム MaterialInstance を叩くと、
        // 盤面の全対象ぶんの無駄書きになる。プレビューが動いたときだけ書く。
        if (m_paintProgress > 0.0f || previewFaded) ApplyVisual();
        return;
    }

    m_remaining -= dt;
    if (m_remaining <= 0.0f) {
        ClearPolarity();
        debugRemaining = 0.0f;
        return;
    }

    debugRemaining = m_remaining;
    ApplyVisual();
}

inline void PolarityTargetComponent::ApplyVisual()
{
    const MaterialInstance instance = material.Instance(static_cast<uint32_t>(emissiveSlot));
    if (!instance.IsValid()) return;

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

    // 6.5「照射中はビームに触れている敵へリアルタイムでプレビューを出す」。
    // 事故が続くとストレスになる、と名指しされている表示なので、塗り終わってから
    // ではなく塗っている最中に、これから何になるかを見せる。
    if (m_paintProgress > 0.0f && m_paintPolarity != Polarity::None) {
        const float fill = Clamp01(m_paintProgress);
        if (WouldNeutralize(m_paintPolarity)) {
            // 逆極 = 中和。極の色へ寄せると「上書きされる」に見えるため、
            // どちらの極でもない警告色へ倒し、速い明滅で異常であることを出す。
            const float alarm = std::sin(Time::time * 12.0f * TWO_PI) * 0.5f + 0.5f;
            color = kColorWarning;
            scale = Max(scale, kEmissiveBase * Lerp(0.4f, 1.6f, alarm) * fill);
        } else {
            // 無極 → 付与 / 同極 → 延長。どちらも「これから乗る極の色が満ちていく」。
            const Vector4 target = PolarityColor(m_paintPolarity);
            color = color + (target - color) * fill;
            scale = Max(scale, kEmissiveBase * fill);
        }
    }

    instance.SetVector3(kEmissiveColorId, { color.x, color.y, color.z });
    instance.SetFloat(kEmissiveScaleId, scale);
}

} // namespace sandbox
