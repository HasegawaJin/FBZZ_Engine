/// @file    UiHeroTextComponent.hpp
/// @brief   題字 (UITextHero.mat の UIText) へ、出現の溶け込みと周期の走査を流す
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// 使い方:
///   Targets に UITextHero.mat を割り当てた UIText の名前を並べる。文言はシーン
///   (または他のスクリプトの SetText) が正本で、ここは最初の更新で控えてから
///   制御値付きの文字列を流し続ける。色 (UIText.color) は触らない ─ 全体の α や
///   色ずれは持ち主 (ResultPresenter など) が今までどおり SetTextColor で決める。
///
/// WHY 最初の OnUpdate で文言を控えるか (OnStart でないか):
///   CLEAR / FAILED は ResultPresenter が OnStart で入れる。スクリプトの開始順は
///   決まっていないので、こちらの OnStart で読むと «Text» のままのことがある。
///   全員の OnStart が済んだ最初の更新で読めば、誰が先でも正しい文言になる。
///
/// WHY 走査を一定間隔で 1 本だけか (ScreenDressing の艶と同じ判断):
///   ずっと光っている題字は «看板» になる。間隔を空けて 1 本走れば «ときどき
///   光を拾った» に見える。間隔は艶 (6.5 秒) より短くしてある ─ 題字は画面の
///   主役で、少しだけ多く動いてよい。
#pragma once

#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace fbzz::scene;

namespace sandbox {

class UiHeroTextComponent : public Script {
    FBZZ_SCRIPT(UiHeroTextComponent)

public:
    FBZZ_GROUP("対象")
    FBZZ_LIST_FIELD(std::string, targets, "対象")
    FBZZ_TOOLTIP("UITextHero.mat を割り当てた UIText の名前。上から順に出る")

    FBZZ_GROUP("導入")
    FBZZ_FIELD_RANGE(float, introDelay, 0.15f, "遅延", 0.0f, 3.0f)
    FBZZ_FIELD_RANGE(float, introStagger, 0.18f, "のけぞり", 0.0f, 2.0f)
    FBZZ_FIELD_RANGE(float, introSeconds, 0.85f, "継続時間", 0.05f, 3.0f)
    FBZZ_TOOLTIP("粒が寄り集まって字になるまでの秒数")

    FBZZ_GROUP("薙ぎ")
    FBZZ_FIELD_RANGE(float, sweepInterval, 4.2f, "間隔", 0.5f, 30.0f)
    FBZZ_FIELD_RANGE(float, sweepSeconds, 0.9f, "薙ぎ", 0.1f, 5.0f)
    FBZZ_FIELD_RANGE(float, sweepWidth, 0.35f, "幅", 0.05f, 1.0f)
    FBZZ_FIELD_RANGE(float, sweepStagger, 0.12f, "のけぞり", 0.0f, 1.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugBound, 0, "Bound")

    void OnStart() override;
    void OnUpdate() override;

    /// 出現をやり直す (ページが変わったときなど)。
    void Replay() { m_elapsed = 0.0f; m_sweep = -sweepInterval * 0.5f; }

private:
    struct Target {
        GameObject* go = nullptr;
        std::string plain;   ///< 素の文言。空なら未取得
        std::string last;    ///< 最後に流した文字列
    };
    std::vector<Target> m_targets;
    float m_elapsed = 0.0f;
    float m_sweep   = 0.0f;   ///< 走査の周期の中の位置 [秒]
    bool  m_bound   = false;

    void Bind();
};

FBZZ_REFLECT(UiHeroTextComponent)

inline void UiHeroTextComponent::OnStart()
{
    m_targets.clear();
    for (const std::string& name : targets) {
        if (name.empty()) continue;
        Target t;
        t.go = scene.Find(name);
        if (!t.go) { debug.LogWarning("UiHeroText: '" + name + "' が見つかりません"); continue; }
        if (auto* text = t.go->GetComponent<UIText>()) text->richText = true;
        m_targets.push_back(t);
    }
    m_elapsed = 0.0f;
    m_sweep   = -sweepInterval * 0.5f;   // 出現の直後に走らせない (出現と走査が重なると読めない)
    m_bound   = false;
}

inline void UiHeroTextComponent::Bind()
{
    m_bound = true;
    int bound = 0;
    for (Target& t : m_targets) {
        auto* text = t.go ? t.go->GetComponent<UIText>() : nullptr;
        if (!text) continue;
        // 既に制御値付きなら (再バインド)、素の文言は持っているはず。
        if (text->text.find("<color=") == std::string::npos) t.plain = text->text;
        if (!t.plain.empty()) ++bound;
    }
    debugBound = bound;
}

inline void UiHeroTextComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);
    if (!m_bound) Bind();
    m_elapsed += dt;

    const float interval = (std::max)(sweepInterval, 0.5f);
    m_sweep += dt;
    if (m_sweep >= interval) m_sweep -= interval;

    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        Target& t = m_targets[i];
        if (!t.go || t.plain.empty()) continue;
        const float reveal = uimotion::Stagger(m_elapsed - introDelay, static_cast<int>(i),
                                               introStagger, introSeconds);
        // 走査の頭。走っていない間は範囲外 (9) に置く ─ どの文字にも掛からない。
        const float local = (m_sweep - sweepStagger * static_cast<float>(i)) / (std::max)(sweepSeconds, 0.1f);
        const float head  = (local <= 0.0f || local >= 1.0f) ? 9.0f : -0.4f + 1.8f * local;
        // 出現の途中は走査を掛けない (溶け込みの縁が熱色なので、走査まで足すと白飛び)。
        std::string rich = textfx::Hero(t.plain, uimotion::OutQuint(reveal),
                                        reveal >= 1.0f ? head : 9.0f, sweepWidth);
        if (rich != t.last) { ui.SetText(t.go, rich); t.last = std::move(rich); }
    }
}

} // namespace sandbox
