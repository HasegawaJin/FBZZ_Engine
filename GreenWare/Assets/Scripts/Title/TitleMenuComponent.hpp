/// @file    TitleMenuComponent.hpp
/// @brief   タイトルメニュー。行ウィジェットの状態を見た目と遷移へつなぐ
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 画面の作り:
///   MenuRow_`<NAME>` (UIButton) ─┬─ Band  (UIImage + UIMenuBand.mat)  行の背後の選択帯
///                              ├─ Bar   (UIImage + UIMenuItem.mat)  左端の芯
///                              └─ Label (UIText)
///
/// @note 選択行は保持しない: 「カーソルがどの行の上か」を UISystem が UIButton の状態として
///       持っており、スクリプト側で index を別に持つとずれたときに正しさを判断できなくなる。
/// @note 当たり判定は行全体に置く: バーの 5px や文字の字面だけだと当たりにくい。UISystem は
///       子から先に判定するので、行の UIButton 1 つでバー・ラベルをまとめて押せる領域にできる。
/// @note 素材差し替えでなくマテリアルで作る: Menu_Bar_dim/_red/_blue は形が同じで色と発光だけ
///       違う。テクスチャ差し替えは 0/1 でしか表現できず光が飛ぶ。UIMenuItem.hlsl は芯の色と
///       裾の強さを別に持つので selected を補間できる。
/// @note 行の出現もここで書く (UiRevealComponent に任せない): Label の色は毎フレームここが
///       書くため、出現の α を別スクリプトが同じ Label へ書くと 2 つが交互に勝ってちらつく。
/// @note 文字は 1 文字ずつ動かす (UiTextFx): 出現は «解読»、ホバー中は «走査» で、1 色塗りの
///       ままだと装置の表示に見えない。走査はホバー中だけに絞る (常時だと選択の合図にならない)。
/// @note ホバーで文字を寄せる (色だけにしない): 色の変化だけでは «光った» としか読めないが、
///       数 px 押し出すと «行を押した» ように見える。戻りは寄りより遅くする (release > blend)。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiNavSe.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <Scripts/Utils/BgmLibrary.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

using namespace fbzz::scene;

namespace sandbox {

class TitleMenuComponent : public Script {
    FBZZ_SCRIPT(TitleMenuComponent)

public:
    FBZZ_GROUP("見た目")
    FBZZ_FIELD_COLOR(labelDimColor, (::fbzz::math::Vector4{ 0.462745f, 0.454902f, 0.439216f, 1.0f }),
                     "Label Dim")
    FBZZ_TOOLTIP("カーソルが乗っていない行の文字色。Reference の rgb(118,116,112)")
    FBZZ_FIELD_COLOR(labelActiveColor, (::fbzz::math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }),
                     "Label Active")
    FBZZ_FIELD_RANGE(float, blendSeconds, 0.09f, "ブレンド", 0.0f, 1.0f)
    FBZZ_TOOLTIP("光が寄ってくる速さ。0 で瞬間的に切り替わる")
    FBZZ_FIELD_RANGE(float, releaseSeconds, 0.18f, "解放", 0.0f, 1.0f)
    FBZZ_TOOLTIP("カーソルが離れたあと消えるまでの速さ。寄りより遅くする")
    FBZZ_FIELD(float, hoverNudge, 10.0f, "押し出し")
    FBZZ_TOOLTIP("ホバー中に文字を右へ押し出す量 [px]。0 で動かない")

    FBZZ_GROUP("導入")
    FBZZ_FIELD_RANGE(float, introDelay, 0.55f, "遅延", 0.0f, 3.0f)
    FBZZ_TOOLTIP("画面に入ってから最初の行が動き始めるまで。題字の出現 (UiReveal) の後に")
    FBZZ_FIELD_RANGE(float, introStagger, 0.09f, "のけぞり", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, introSeconds, 0.46f, "継続時間", 0.05f, 3.0f)
    FBZZ_FIELD(float, introSlide, -40.0f, "滑り")
    FBZZ_TOOLTIP("出る前の位置のずれ [px]。負で左から滑り込む")

    FBZZ_GROUP("流れ")
    FBZZ_FIELD(std::string, startScene,   "StageSelect", "START")
    FBZZ_FIELD(std::string, optionsScene, "Options", "OPTIONS")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugHovered, "-", "ホバー中")
    FBZZ_FIELD_READ_ONLY(float, debugIntro, 0.0f, "導入")

    void OnStart() override;
    void OnUpdate() override;

private:
    bool m_wipeWriting = false;   ///< 扉を自分で postprocess へ書いたか (transition::Drive)
    static constexpr int kCount = 3;
    /// シーン上の行の名前。ここを変えるならシーン側の GameObject 名も合わせること。
    static constexpr const char* kNames[kCount] = { "START", "OPTIONS", "EXIT" };

    void Submit(int index);
    /// 行 1 本ぶんの見た目を、出現の進行度 × ホバー量から書く。
    void Paint(int i, float reveal);

    /// 行の子を名前で引く。行ごとに Bar / Label という同じ名前を使っているので、
    /// シーン全体から引く scene.Find では、どの行のものか決まらない。
    [[nodiscard]] static GameObject* Child(GameObject* parent, std::string_view name);

    GameObject* m_rows[kCount]   = {};
    GameObject* m_bands[kCount]  = {};
    GameObject* m_bars[kCount]   = {};
    GameObject* m_labels[kCount] = {};
    ::fbzz::math::Vector3 m_rowOrigin[kCount]   = {};
    ::fbzz::math::Vector3 m_labelOrigin[kCount] = {};
    float m_amount[kCount] = {};   ///< ホバー量 0..1
    float m_flash[kCount]  = {};   ///< 決定の光。1 から 0 へ減衰
    bool  m_lit[kCount]    = {};   ///< 前フレームに点いていたか (乗った瞬間の検出)
    float m_intro = 0.0f;          ///< 画面に入ってからの秒数
    float m_clock = 0.0f;          ///< 帯の斜線の位相
    std::string m_text[kCount];    ///< ラベルの素の文言 (シーンが正本)
    std::string m_rich[kCount];    ///< 最後に流し込んだ文字列。同じなら書かない
    float m_litSince[kCount] = {}; ///< 点いてからの秒数 (走査の位相)
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
    se::EnsureSource(scene, "UI");
    bgm::Play(audio, bgm::kTitle);
    for (int i = 0; i < kCount; ++i) {
        m_rows[i]   = scene.Find(std::string("MenuRow_") + kNames[i]);
        m_bands[i]  = Child(m_rows[i], "Band");
        m_bars[i]   = Child(m_rows[i], "Bar");
        m_labels[i] = Child(m_rows[i], "Label");
        if (!m_rows[i] || !m_bars[i] || !m_labels[i]) {
            debug.LogWarning(std::string("TitleMenuComponent: MenuRow_") + kNames[i] +
                             " (と子の Bar / Label) が見つかりません");
        }
        /// @note Band は後から足した子。無くても行は動く (帯が出ないだけ)。
        if (m_rows[i])   m_rowOrigin[i]   = m_rows[i]->transform.position;
        if (m_labels[i]) {
            m_labelOrigin[i] = m_labels[i]->transform.position;
            /// @note 色付きの文字列を流すので richText を立てる。素の文言は控えておく。
            if (auto* t = m_labels[i]->GetComponent<UIText>()) {
                t->richText = true;
                m_text[i]   = t->text;
            }
        }
        m_rich[i].clear();
        m_litSince[i] = 0.0f;
        m_amount[i] = 0.0f;
        m_flash[i]  = 0.0f;
        m_lit[i]    = false;
    }
    m_intro = 0.0f;
    m_clock = 0.0f;
    /// @note 1 フレーム目から «出ていない» で描く。OnUpdate を待つと、置いた位置で
    ///       1 フレームだけ見えてから引っ込む。
    for (int i = 0; i < kCount; ++i) Paint(i, 0.0f);
}

inline void TitleMenuComponent::Paint(int i, float reveal)
{
    const float e     = uimotion::OutCubic(reveal);
    const float alpha = uimotion::OutQuint(reveal * 1.25f);
    const float t     = m_amount[i];

    if (m_rows[i]) {
        m_rows[i]->transform.position = {
            m_rowOrigin[i].x + introSlide * (1.0f - e),
            m_rowOrigin[i].y, m_rowOrigin[i].z,
        };
    }
    if (m_bars[i]) {
        ui.SetImageColor(m_bars[i], { 1.0f, 1.0f, 1.0f, alpha });
        ui.SetMaterialFloat(m_bars[i], "selected", t);
    }
    if (m_bands[i]) {
        ui.SetImageColor(m_bands[i], { 1.0f, 1.0f, 1.0f, alpha });
        ui.SetMaterialFloat(m_bands[i], "selected", t);
        ui.SetMaterialFloat(m_bands[i], "flash", m_flash[i]);
        ui.SetMaterialFloat(m_bands[i], "phase", m_clock);
    }
    if (m_labels[i]) {
        /// @note 寄りは OutCubic で «押された» 形に。t は指数で寄るので、そのままだと
        ///       立ち上がりが鈍い。
        const float nudge = hoverNudge * uimotion::OutCubic(t);
        m_labels[i]->transform.position = {
            m_labelOrigin[i].x + nudge, m_labelOrigin[i].y, m_labelOrigin[i].z,
        };
        const ::fbzz::math::Vector4 c = textfx::Mix(labelDimColor, labelActiveColor, t);
        /// @note 文字の色は文字列側 (UiTextFx) が持ち、UIText.color は α だけにする。
        std::string rich;
        if (reveal < 1.0f) {
            /// @note 出現は解読。乱数は 1/30 秒ごとに更新 (毎フレームだと «ノイズ» に見える)。
            const ::fbzz::math::Vector4 scramble = textfx::Mix(labelDimColor, { 0.0f, 0.0f, 0.0f, 1.0f }, 0.35f);
            rich = textfx::Decode(m_text[i], reveal, 0x51ED27u + static_cast<std::uint32_t>(i),
                                  static_cast<std::uint32_t>(m_clock * 30.0f), labelDimColor,
                                  { 1.0f, 1.0f, 1.0f, 1.0f }, scramble);
        } else if (t > 0.01f) {
            /// @note 点いている間は 1.6 秒に 1 本、光が左から右へ字面を舐める。
            const float head = -0.45f + 1.9f * std::fmod(m_litSince[i], 1.6f) / 1.6f;
            rich = textfx::Sweep(m_text[i], c, { 1.0f, 1.0f, 1.0f, 1.0f }, head, 0.38f);
        } else {
            rich = textfx::Wrap(m_text[i], c);
        }
        if (rich != m_rich[i]) { ui.SetText(m_labels[i], rich); m_rich[i] = std::move(rich); }
        ui.SetTextColor(m_labels[i], { 1.0f, 1.0f, 1.0f, c.w * alpha });
    }
}

inline void TitleMenuComponent::OnUpdate()
{
    /// @note 実時間で寄せる。タイトルはヒットストップやスローの影響を受けない。
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);
    m_clock += dt;
    m_intro += dt;
    debugIntro = m_intro;

    /// @note 決定の光は扉が閉まる間も減衰させる (押した行が光ったまま固まらない)。
    for (int i = 0; i < kCount; ++i) uimotion::Decay(m_flash[i], dt, 0.16f);

    /// @note 扉 (ワイプ)。塗っている / 剥がしている最中は入力を受けない。
    ///       見た目だけは書き続ける (帯の斜線が止まると «固まった» に見える)。
    const bool busy = transition::Drive(dt, scene, postprocess, m_wipeWriting, false);

    const float attack  = blendSeconds   <= 0.0f ? 0.0f : blendSeconds;
    const float release = releaseSeconds <= 0.0f ? attack : releaseSeconds;

    debugHovered = "-";
    for (int i = 0; i < kCount; ++i) {
        const float reveal = uimotion::Stagger(m_intro - introDelay, i, introStagger, introSeconds);
        /// @note 置き切る前の行はカーソルを受けない。滑り込んでいる最中に光ると、
        ///       出現と点灯が混ざって «何が起きたか» が読めない。
        const bool ready = reveal >= 1.0f && !busy;

        /// @note 押下中も点灯を維持する。押した瞬間に消えると、押せたのかどうかが判らない。
        const bool lit = ready && m_rows[i] && (ui.IsHovered(m_rows[i]) || ui.IsPressed(m_rows[i]));
        if (lit) debugHovered = kNames[i];
        if (lit && !m_lit[i]) {
            /// @note 乗った瞬間。帯が開くのと同時に、小さく光って音を出す。
            audio.PlayOneShot(uinav::kMove);
            m_flash[i] = (std::max)(m_flash[i], 0.28f);
        }
        m_lit[i] = lit;
        m_litSince[i] = lit ? m_litSince[i] + dt : 0.0f;

        m_amount[i] = uimotion::Approach(m_amount[i], lit ? 1.0f : 0.0f, dt, lit ? attack : release);
        Paint(i, reveal);

        if (ready && ui.WasClicked(m_rows[i])) {
            m_flash[i] = 1.0f;
            /// @note 光った状態を今フレームに出してから扉を開ける
            Paint(i, reveal);
            Submit(i);
            return;
        }
    }
}

inline void TitleMenuComponent::Submit(int index)
{
    /// @note EXIT だけシーンではなくアプリの終了。
    if (index == 2) { app.Quit(); return; }
    audio.PlayOneShot(uinav::kConfirm);

    const std::string& target = (index == 0) ? startScene : optionsScene;
    if (target.empty()) {
        debug.Log(std::string("TitleMenuComponent: ") + kNames[index] + " の行き先が空です");
        return;
    }
    if (!transition::Begin(target))
        debug.LogError("TitleMenuComponent: シーンへの扉を開けません -> " + target);
}

} // namespace sandbox
