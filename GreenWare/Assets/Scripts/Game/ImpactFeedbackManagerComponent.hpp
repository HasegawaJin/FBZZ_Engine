/// @file    ImpactFeedbackManagerComponent.hpp
/// @brief   「何が起きたか」を 1 回受け取り、手触りの反応をまとめて鳴らす
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 束ねるか:
///   ヒットストップ・カメラ揺れ・振動・画面効果・効果音は、常に同じ 1 つの出来事に
///   対して同時に鳴る。呼び出し元が 5 つ別々に叩くと、「敵の衝突をもう少し重くしたい」
///   という 1 つの意図が 5 箇所の編集になり、しかも呼び出し元ごとに配分がずれていく。
///   出来事の種類と強さだけを受け取り、そこから 5 つへ配るのはここの仕事にする。
///
/// WHY 種類を enum で持つか:
///   衝突と被弾と着地では鳴らし方の配分が違う。衝突は止めと揺れ、被弾は画面の赤と振動、
///   着地は軽い揺れだけ、という描き分けを呼び出し元に書かせると、結局そこが調整場所に
///   なってしまう。呼び出し元は「敵がぶつかった」としか言わない。
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/CameraShakeManagerComponent.hpp>
#include <Scripts/Game/HitstopManagerComponent.hpp>
#include <Scripts/Game/RumbleManagerComponent.hpp>
#include <Scripts/Game/ScreenEffectManagerComponent.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

// 手触りを鳴らす出来事の種類。増やすときはここと Profile の対応を 1 対 1 で足す。
enum class FeedbackEvent {
    EnemyImpact,   // 敵どうしの激突
    AnchorImpact,  // 敵を柱・壁へ叩きつけた
    PlayerHurt,    // プレイヤーの被弾
    PlayerLand,    // プレイヤーの着地
};

// 一番弱い出来事でもこれだけは鳴らす。0 まで落とすと、軽い接触が「起きなかったこと」
// になり、当たったのに無反応という一番紛らわしい状態になる。
inline constexpr float kMinImpactVolume = 0.45f;

// 出来事の種類と強さから、鳴らす変奏の束を選ぶ。
//
// WHY 種類ごとの配分表 (Mix) に持たせないか: Mix は Inspector の数値をそのまま束ねた
//     ものなので、強さで分岐する余地が無い。素材が 3 段で入っている衝突だけは
//     強さが選択に効くため、束の選択は別の関数にしてある。
[[nodiscard]] inline const se::Bank& BankFor(FeedbackEvent event, float strength01)
{
    switch (event) {
    case FeedbackEvent::AnchorImpact: return se::kImpactWall;
    case FeedbackEvent::PlayerHurt:   return se::kPlayerDamaged;
    case FeedbackEvent::PlayerLand:   return se::kPlayerLanding;
    case FeedbackEvent::EnemyImpact:  break;
    }
    return se::ImpactByStrength(strength01);
}

class ImpactFeedbackManagerComponent : public Script {
    FBZZ_SCRIPT(ImpactFeedbackManagerComponent)

    // 効果音はこの GameObject から鳴らす。無ければ音だけが出ない。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    // WHY 種類ごとに配分を持つか (共通の 1 セットにしないか):
    //   「強さ」だけを共有して配分まで同じにすると、被弾でも画面が止まり、
    //   敵の激突でも画面が赤くなる。何が起きたかは配分の違いでしか伝わらない。
    FBZZ_GROUP("Enemy Impact")
    FBZZ_FIELD_RANGE(float, impactHitstop, 1.0f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, impactShake,   1.0f, "揺れ",   0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, impactRumble,  1.0f, "振動",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, impactFlash,   0.0f, "閃光",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, impactDistort, 0.35f, "歪み", 0.0f, 1.0f)
    FBZZ_FIELD_AUDIO(impactSfx, "", "効果音")

    FBZZ_GROUP("Anchor Impact")
    FBZZ_FIELD_RANGE(float, anchorHitstop, 1.0f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, anchorShake,   1.35f, "揺れ",  0.0f, 2.0f)
    FBZZ_TOOLTIP("柱への叩きつけは決まり手なので、敵どうしより大きく揺らす")
    FBZZ_FIELD_RANGE(float, anchorRumble,  1.0f, "振動",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, anchorFlash,   0.0f, "閃光",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, anchorDistort, 0.5f, "歪み", 0.0f, 1.0f)
    FBZZ_FIELD_AUDIO(anchorSfx, "", "効果音")

    FBZZ_GROUP("Player Hurt")
    FBZZ_FIELD_RANGE(float, hurtHitstop, 0.35f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_TOOLTIP("被弾で強く止めると、逃げる操作を奪われた感じになる。浅めに留める")
    FBZZ_FIELD_RANGE(float, hurtShake,   0.7f, "揺れ",   0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, hurtRumble,  0.9f, "振動",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hurtFlash,   1.0f, "閃光",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hurtDistort, 0.8f, "歪み", 0.0f, 1.0f)
    FBZZ_FIELD_AUDIO(hurtSfx, "", "効果音")

    // WHY 着地に止めも赤も割り当てないか: 着地は自分の操作の結末であって、
    //     受けた出来事ではない。画面を止めると自分の移動を邪魔されたことになり、
    //     赤く光ると被弾と読み違える。伝えたいのは重さだけなので揺れと振動に絞る。
    FBZZ_GROUP("Player Land")
    FBZZ_FIELD_RANGE(float, landHitstop, 0.0f, "ヒットストップ", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, landShake,   0.45f, "揺れ",  0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, landRumble,  0.4f, "振動",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, landFlash,   0.0f, "閃光",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, landDistort, 0.0f, "歪み", 0.0f, 1.0f)
    FBZZ_FIELD_AUDIO(landSfx, "", "効果音")

    FBZZ_GROUP("Shared")
    FBZZ_FIELD_RANGE(float, distortSeconds, 0.3f, "Distortion Seconds", 0.0f, 2.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugLast, "", "Last Event")

    [[nodiscard]] static ImpactFeedbackManagerComponent* Instance() { return s_instance; }

    // 呼び出し元はこの 1 本だけを使う。strength01 は「どれくらい強い出来事か」。
    void Play(FeedbackEvent event, float strength01);

    void OnStart() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }

private:
    static inline ImpactFeedbackManagerComponent* s_instance = nullptr;

    struct Mix {
        float hitstop = 0.0f;
        float shake   = 0.0f;
        float rumble  = 0.0f;
        float flash   = 0.0f;
        float distort = 0.0f;
        const std::string* sfx = nullptr;
        const char* name = "";
    };

    [[nodiscard]] Mix MixFor(FeedbackEvent event) const;
};

FBZZ_REFLECT(ImpactFeedbackManagerComponent)

inline void ImpactFeedbackManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("ImpactFeedbackManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;
    // 手触りをまとめて鳴らす立場なので、音も 1 箇所から出す。
    // 出来事は画面の中心で起きたものとして扱うため 2D (距離で薄くならない)。
    se::EnsureSource(scene);
}

inline ImpactFeedbackManagerComponent::Mix
ImpactFeedbackManagerComponent::MixFor(FeedbackEvent event) const
{
    switch (event) {
    case FeedbackEvent::AnchorImpact:
        return { anchorHitstop, anchorShake, anchorRumble, anchorFlash, anchorDistort,
                 &anchorSfx, "AnchorImpact" };
    case FeedbackEvent::PlayerHurt:
        return { hurtHitstop, hurtShake, hurtRumble, hurtFlash, hurtDistort,
                 &hurtSfx, "PlayerHurt" };
    case FeedbackEvent::PlayerLand:
        return { landHitstop, landShake, landRumble, landFlash, landDistort,
                 &landSfx, "PlayerLand" };
    case FeedbackEvent::EnemyImpact:
        break;
    }
    return { impactHitstop, impactShake, impactRumble, impactFlash, impactDistort,
             &impactSfx, "EnemyImpact" };
}

inline void ImpactFeedbackManagerComponent::Play(FeedbackEvent event, float strength01)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;

    const Mix mix = MixFor(event);
    debugLast = mix.name;

    // 個々のマネージャーが居なくても他は鳴らす。1 つ外しただけで全部無音になると、
    // どれが原因で手応えが消えたのか分からなくなる。
    if (auto* hitstop = HitstopManagerComponent::Instance())
        hitstop->Hit(strength * mix.hitstop);

    if (auto* shake = CameraShakeManagerComponent::Instance())
        shake->Shake(strength * mix.shake);

    if (auto* pad = RumbleManagerComponent::Instance())
        pad->Rumble(strength * mix.rumble);

    if (auto* screen = ScreenEffectManagerComponent::Instance()) {
        if (mix.flash > 0.0f)   screen->Flash(strength * mix.flash);
        if (mix.distort > 0.0f) screen->Distort(strength * mix.distort, distortSeconds);
    }

    // 音量も強さに乗せる。段が 3 つしかないので、段の中の差は倍率で埋める。
    // WHY 段だけに任せないか: 0.34 と 0.66 は同じ Mid の音になるが、出来事としては
    //     倍近く違う。段の切り替えだけだと、しきい値をまたぐ瞬間だけ急に重くなる。
    const float volume = Lerp(kMinImpactVolume, 1.0f, strength);
    se::Play(audio, *mix.sfx, BankFor(event, strength), volume);
}

} // namespace sandbox
