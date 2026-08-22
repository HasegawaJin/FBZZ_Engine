/// @file ImpactFeedbackManagerComponent.hpp
/// @brief 「何が起きたか」を 1 回受け取り、手触りの反応をまとめて鳴らす
/// @author Hasegawa Jin
/// @date 2026-08-22
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

class ImpactFeedbackManagerComponent : public Script {
    FBZZ_SCRIPT(ImpactFeedbackManagerComponent)

    // 効果音はこの GameObject から鳴らす。無ければ音だけが出ない。
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    // WHY 種類ごとに配分を持つか (共通の 1 セットにしないか):
    //   「強さ」だけを共有して配分まで同じにすると、被弾でも画面が止まり、
    //   敵の激突でも画面が赤くなる。何が起きたかは配分の違いでしか伝わらない。
    FBZZ_GROUP("Enemy Impact")
    FBZZ_FIELD_RANGE(float, impactHitstop, 1.0f, "Hitstop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, impactShake,   1.0f, "Shake",   0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, impactRumble,  1.0f, "Rumble",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, impactFlash,   0.0f, "Flash",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, impactDistort, 0.35f, "Distortion", 0.0f, 1.0f)
    FBZZ_FIELD_FILE(impactSfx, "", "SFX", ".wav,.ogg")

    FBZZ_GROUP("Anchor Impact")
    FBZZ_FIELD_RANGE(float, anchorHitstop, 1.0f, "Hitstop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, anchorShake,   1.35f, "Shake",  0.0f, 2.0f)
    FBZZ_TOOLTIP("柱への叩きつけは決まり手なので、敵どうしより大きく揺らす")
    FBZZ_FIELD_RANGE(float, anchorRumble,  1.0f, "Rumble",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, anchorFlash,   0.0f, "Flash",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, anchorDistort, 0.5f, "Distortion", 0.0f, 1.0f)
    FBZZ_FIELD_FILE(anchorSfx, "", "SFX", ".wav,.ogg")

    FBZZ_GROUP("Player Hurt")
    FBZZ_FIELD_RANGE(float, hurtHitstop, 0.35f, "Hitstop", 0.0f, 1.0f)
    FBZZ_TOOLTIP("被弾で強く止めると、逃げる操作を奪われた感じになる。浅めに留める")
    FBZZ_FIELD_RANGE(float, hurtShake,   0.7f, "Shake",   0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, hurtRumble,  0.9f, "Rumble",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hurtFlash,   1.0f, "Flash",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, hurtDistort, 0.8f, "Distortion", 0.0f, 1.0f)
    FBZZ_FIELD_FILE(hurtSfx, "", "SFX", ".wav,.ogg")

    // WHY 着地に止めも赤も割り当てないか: 着地は自分の操作の結末であって、
    //     受けた出来事ではない。画面を止めると自分の移動を邪魔されたことになり、
    //     赤く光ると被弾と読み違える。伝えたいのは重さだけなので揺れと振動に絞る。
    FBZZ_GROUP("Player Land")
    FBZZ_FIELD_RANGE(float, landHitstop, 0.0f, "Hitstop", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, landShake,   0.45f, "Shake",  0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, landRumble,  0.4f, "Rumble",  0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, landFlash,   0.0f, "Flash",   0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, landDistort, 0.0f, "Distortion", 0.0f, 1.0f)
    FBZZ_FIELD_FILE(landSfx, "", "SFX", ".wav,.ogg")

    FBZZ_GROUP("Shared")
    FBZZ_FIELD_RANGE(float, distortSeconds, 0.3f, "Distortion Seconds", 0.0f, 2.0f)

    FBZZ_GROUP("Debug")
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

    if (auto* rumble = RumbleManagerComponent::Instance())
        rumble->Rumble(strength * mix.rumble);

    if (auto* screen = ScreenEffectManagerComponent::Instance()) {
        if (mix.flash > 0.0f)   screen->Flash(strength * mix.flash);
        if (mix.distort > 0.0f) screen->Distort(strength * mix.distort, distortSeconds);
    }

    if (mix.sfx && !mix.sfx->empty()) audio.PlayOneShot(*mix.sfx);
}

} // namespace sandbox
