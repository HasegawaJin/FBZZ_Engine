/// @file TitleMenuComponent.hpp
/// @brief タイトルメニュー。行ウィジェットの状態を見た目と遷移へつなぐ
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// 画面の作り:
///   MenuRow_<NAME> (UIButton) ─┬─ Bar   (UIImage + UIMenuItem.mat)
///                              └─ Label (UIText)
///
/// WHY index を持たないか:
///   入力は [[GameCursorComponent]] のカーソル 1 本に寄せてある。どの行が選ばれて
///   いるかは「カーソルがどの行の上にあるか」で既に決まっていて、UISystem が
///   UIButton の状態として持っている。スクリプトが別に index を持つと、
///   カーソルとずれた瞬間にどちらが正しいのか判断できなくなる。
///
/// WHY 行を親にするか:
///   当たり判定をバーの 5px や文字の字面ではなく行全体にしたい。UISystem は
///   子から先に判定してイベントを消費するので、行に置いた UIButton 1 つで
///   バーとラベルをまとめて 1 つの押せる領域にできる。
///
/// WHY 素材の差し替えではなくマテリアルで作るか:
///   Menu_Bar_dim / _red / _blue は芯の形が同じで、色と発光の有無だけが違う。
///   テクスチャを張り替えると 0 か 1 しか表現できず、光が「パッ」と飛ぶ。
///   UIMenuItem.hlsl は芯の色と裾の強さを別々に持つので、selected を補間できる。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

using namespace fbzz::scene;

namespace sandbox {

class TitleMenuComponent : public Script {
    FBZZ_SCRIPT(TitleMenuComponent)

public:
    FBZZ_GROUP("Look")
    FBZZ_FIELD_COLOR(labelDimColor, (::fbzz::math::Vector4{ 0.462745f, 0.454902f, 0.439216f, 1.0f }),
                     "Label Dim")
    FBZZ_TOOLTIP("カーソルが乗っていない行の文字色。Reference の rgb(118,116,112)")
    FBZZ_FIELD_COLOR(labelActiveColor, (::fbzz::math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }),
                     "Label Active")
    FBZZ_FIELD_RANGE(float, blendSeconds, 0.09f, "Blend", 0.0f, 1.0f)
    FBZZ_TOOLTIP("光が寄ってくる速さ。0 で瞬間的に切り替わる")

    FBZZ_GROUP("Flow")
    FBZZ_FIELD(std::string, startScene,   "Main",    "START")
    FBZZ_FIELD(std::string, optionsScene, "Options", "OPTIONS")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugHovered, "-", "Hovered")

    void OnStart() override;
    void OnUpdate() override;

private:
    static constexpr int kCount = 3;
    // シーン上の行の名前。ここを変えるならシーン側の GameObject 名も合わせること。
    static constexpr const char* kNames[kCount] = { "START", "OPTIONS", "EXIT" };

    void Submit(int index);

    /// 行の子を名前で引く。行ごとに Bar / Label という同じ名前を使っているので、
    /// シーン全体から引く scene.Find では、どの行のものか決まらない。
    [[nodiscard]] static GameObject* Child(GameObject* parent, std::string_view name);

    GameObject* m_rows[kCount]   = {};
    GameObject* m_bars[kCount]   = {};
    GameObject* m_labels[kCount] = {};
    float m_amount[kCount] = {};
};

FBZZ_REFLECT(TitleMenuComponent)

inline GameObject* TitleMenuComponent::Child(GameObject* parent, std::string_view name)
{
    if (!parent) return nullptr;
    for (int i = 0, n = parent->GetChildCount(); i < n; ++i)
        if (GameObject* child = parent->GetChild(i); child && child->name == name) return child;
    return nullptr;
}

inline void TitleMenuComponent::OnStart()
{
    for (int i = 0; i < kCount; ++i) {
        m_rows[i]   = scene.Find(std::string("MenuRow_") + kNames[i]);
        m_bars[i]   = Child(m_rows[i], "Bar");
        m_labels[i] = Child(m_rows[i], "Label");
        if (!m_rows[i] || !m_bars[i] || !m_labels[i]) {
            debug.LogWarning(std::string("TitleMenuComponent: MenuRow_") + kNames[i] +
                             " (と子の Bar / Label) が見つかりません");
        }
        m_amount[i] = 0.0f;
    }
}

inline void TitleMenuComponent::OnUpdate()
{
    // 実時間で寄せる。タイトルはヒットストップやスローの影響を受けない。
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);
    const float response = blendSeconds <= 0.0f ? 1.0f : 1.0f - std::exp(-dt / blendSeconds);

    debugHovered = "-";
    for (int i = 0; i < kCount; ++i) {
        if (!m_rows[i]) continue;

        // 押下中も点灯を維持する。押した瞬間に消えると、押せたのかどうかが判らない。
        const bool lit = ui.IsHovered(m_rows[i]) || ui.IsPressed(m_rows[i]);
        if (lit) debugHovered = kNames[i];

        const float target = lit ? 1.0f : 0.0f;
        m_amount[i] += (target - m_amount[i]) * response;
        if (std::abs(target - m_amount[i]) < 0.001f) m_amount[i] = target;

        if (m_bars[i]) ui.SetMaterialFloat(m_bars[i], "selected", m_amount[i]);
        if (m_labels[i]) {
            const float t = m_amount[i];
            ui.SetTextColor(m_labels[i], {
                labelDimColor.x + (labelActiveColor.x - labelDimColor.x) * t,
                labelDimColor.y + (labelActiveColor.y - labelDimColor.y) * t,
                labelDimColor.z + (labelActiveColor.z - labelDimColor.z) * t,
                labelDimColor.w + (labelActiveColor.w - labelDimColor.w) * t,
            });
        }

        if (ui.WasClicked(m_rows[i])) Submit(i);
    }
}

inline void TitleMenuComponent::Submit(int index)
{
    // EXIT だけシーンではなくアプリの終了。
    if (index == 2) { app.Quit(); return; }

    const std::string& target = (index == 0) ? startScene : optionsScene;
    if (target.empty()) {
        debug.Log(std::string("TitleMenuComponent: ") + kNames[index] + " の行き先が空です");
        return;
    }
    if (!scene.LoadScene(target))
        debug.LogError("TitleMenuComponent: シーンを読み込めません -> " + target);
}

} // namespace sandbox
