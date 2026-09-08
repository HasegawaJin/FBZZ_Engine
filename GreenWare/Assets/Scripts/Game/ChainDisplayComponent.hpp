/// @file    ChainDisplayComponent.hpp
/// @brief   続いている連鎖の長さを HUD へ出す
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY 数えるのは CombatManager か:
///   連鎖の長さは衝突を数えた結果で、戦果の一種。表示側が数えると、HUD を切った
///   だけで記録まで消える。ここは受け取った数を文字にするところまでを持つ。
///
/// WHY 1 のときは出さないか:
///   1 は「1 体に当たった」でしかなく、連鎖ではない。当たるたびに数字が出ると、
///   出ていること自体が情報でなくなり、本当に繋がったときの 5 や 6 が埋もれる。
///
/// WHY 猶予の終わりで消すか:
///   連鎖が切れた瞬間に数字が消えると、何連鎖で終わったのかを読む時間が無い。
///   猶予の残りに合わせて薄くしていけば、消えていく過程がそのまま
///   「もう次を当てないと切れる」という残り時間の表示になる。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/CombatManagerComponent.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ChainDisplayComponent : public Script {
    FBZZ_SCRIPT(ChainDisplayComponent)

public:
    FBZZ_GROUP("Binding")
    FBZZ_REF(GameObject, countText, "Count")
    FBZZ_TOOLTIP("連鎖数を出す UIText。未設定なら HUD_Chain を名前で拾う")

    FBZZ_GROUP("見た目")
    FBZZ_FIELD(std::string, prefix, "CHAIN ", "Prefix")
    FBZZ_TOOLTIP("数字の前に付ける文字。空にすると数字だけになる")
    FBZZ_FIELD_COLOR(color, (Vector4{ 1.0f, 0.92f, 0.62f, 1.0f }), "Color")
    FBZZ_FIELD_RANGE_INT(int, minimumChain, 2, "Minimum Chain", 1, 10)
    FBZZ_TOOLTIP("これ未満は出さない。1 は「当たった」であって連鎖ではない")
    FBZZ_FIELD_RANGE(float, fadeBelow, 0.35f, "Fade Below", 0.0f, 1.0f)
    FBZZ_TOOLTIP("猶予の残りがこの割合を切ってから薄くなり始める")
    FBZZ_FIELD_RANGE(float, popBoost, 0.6f, "はじけ", 0.0f, 2.0f)
    FBZZ_TOOLTIP("数字が伸びた瞬間の明るさの上乗せ。0 で光らせない")
    FBZZ_FIELD_RANGE(float, popSeconds, 0.14f, "Pop Seconds", 0.0f, 1.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugShown, 0, "Shown Chain")

    void OnStart() override;
    void OnLateUpdate() override;

private:
    static constexpr const char* kCountName = "HUD_Chain";

    [[nodiscard]] GameObject* Count() const;

    EntityRef m_count;
    int   m_lastChain = 0;
    float m_popRemaining = 0.0f;
    /// 直前に出していたか。消えている間は毎フレーム同じ文字を書き直さない。
    bool  m_visible = false;
};

FBZZ_REFLECT(ChainDisplayComponent)

inline GameObject* ChainDisplayComponent::Count() const
{
    return m_count.Resolve(scene);
}

inline void ChainDisplayComponent::OnStart()
{
    // 参照の解決は 1 度だけ。毎フレーム名前で探すと、見つからない構成のときに
    // 静かにシーン全体の走査を続けることになる。
    GameObject* text = countText.Get();
    if (!text) text = scene.Find(kCountName);
    if (!text) {
        debug.LogError("ChainDisplayComponent: chain text not found (assign it, or name it "
                       "HUD_Chain in the scene).");
        return;
    }
    m_count = EntityRef{ text->GetID() };

    m_lastChain    = 0;
    m_popRemaining = 0.0f;
    m_visible      = false;
    if (GameObject* object = Count()) object->SetActive(false);
}

inline void ChainDisplayComponent::OnLateUpdate()
{
    GameObject* text = Count();
    if (!text) return;

    // WHY 実時間か: 連鎖が伸びる瞬間には必ずヒットストップが掛かる。縮んだ時間で
    //     光らせると、一番見せたい一撃の立ち上がりだけが遅れて出る。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);
    m_popRemaining = std::max(0.0f, m_popRemaining - dt);

    const auto* combat = CombatManagerComponent::Instance();
    const int chain = combat ? combat->ChainCount() : 0;
    const bool wanted = GameSettingsComponent::GameOrDefault().chain
                     && chain >= std::max(minimumChain, 1);

    if (!wanted) {
        if (m_visible) {
            text->SetActive(false);
            m_visible = false;
        }
        m_lastChain = chain;
        debugShown  = 0;
        return;
    }

    if (chain > m_lastChain) m_popRemaining = std::max(popSeconds, 0.0f);
    m_lastChain = chain;

    if (!m_visible) {
        text->SetActive(true);
        m_visible = true;
    }
    ui.SetText(text, prefix + std::to_string(chain));

    // 猶予の残りが fadeBelow を切ってから薄くする。それまでは濃さを変えない。
    const float remaining = combat ? combat->ChainRemaining01() : 0.0f;
    const float fade = fadeBelow <= EPSILON ? 1.0f
                                            : Clamp01(remaining / std::max(fadeBelow, EPSILON));

    float brightness = 1.0f;
    if (popSeconds > EPSILON && m_popRemaining > 0.0f)
        brightness += popBoost * Clamp01(m_popRemaining / popSeconds);

    ui.SetTextColor(text, { color.x * brightness, color.y * brightness, color.z * brightness,
                            color.w * fade });
    debugShown = chain;
}

} // namespace sandbox
