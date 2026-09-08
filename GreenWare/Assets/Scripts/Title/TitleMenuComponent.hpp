/// @file    TitleMenuComponent.hpp
/// @brief   タイトルメニュー。行ウィジェットの状態を見た目と遷移へつなぐ
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 画面の作り:
///   MenuRow_<NAME> (UIButton) ─┬─ Band  (UIImage + UIMenuBand.mat)  行の背後の選択帯
///                              ├─ Bar   (UIImage + UIMenuItem.mat)  左端の芯
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
///
/// WHY 行の出現をここが持つか (UiRevealComponent に任せないか):
///   Label の色は毎フレームここが書く (ホバーで白へ寄せる)。出現の α を別の
///   スクリプトが同じ Label へ書くと、2 つが交互に勝ってちらつく。行に関わる
///   色は 1 か所から出す ─ 出現 × ホバーを 1 つの式にまとめてここで書く。
///
/// WHY 文字の色を 1 文字ずつ動かすか (UiTextFx):
///   出現は «解読» (左から確定していく)、ホバー中は «走査» (光が字面を舐める)。
///   1 色で塗った文字は、どれだけ動かしても «ラベル» のままで、装置の表示に
///   見えない。文字の中に時間が流れていると、画面が «生きている» 側に寄る。
///   走査はホバー中だけ ─ 常時だと全行が光っていて選択の合図にならない。
///
/// WHY ホバーで文字を «寄せる» か (色だけにしないか):
///   色の変化は «光った» としか読めない。文字が芯の側から数 px 押し出されると、
///   帯が «行を押した» ように見えて、行が物として存在している感じが出る。
///   戻りは寄りより遅く (置いた物が戻るのは、押されるより静か)。
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiNavSe.hpp>
#include <Scripts/UI/UiTextFx.hpp>
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
    // シーン上の行の名前。ここを変えるならシーン側の GameObject 名も合わせること。
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
    for (int i = 0; i < kCount; ++i) {
        m_rows[i]   = scene.Find(std::string("MenuRow_") + kNames[i]);
        m_bands[i]  = Child(m_rows[i], "Band");
        m_bars[i]   = Child(m_rows[i], "Bar");
        m_labels[i] = Child(m_rows[i], "Label");
        if (!m_rows[i] || !m_bars[i] || !m_labels[i]) {
            debug.LogWarning(std::string("TitleMenuComponent: MenuRow_") + kNames[i] +
                             " (と子の Bar / Label) が見つかりません");
        }
        // Band は後から足した子。無くても行は動く (帯が出ないだけ)。
        if (m_rows[i])   m_rowOrigin[i]   = m_rows[i]->transform.position;
        if (m_labels[i]) {
            m_labelOrigin[i] = m_labels[i]->transform.position;
            // 色付きの文字列を流すので richText を立てる。素の文言は控えておく。
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
    // 1 フレーム目から «出ていない» で描く。OnUpdate を待つと、置いた位置で
    // 1 フレームだけ見えてから引っ込む。
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
        // 寄りは OutCubic で «押された» 形に。t は指数で寄るので、そのままだと
        // 立ち上がりが鈍い。
        const float nudge = hoverNudge * uimotion::OutCubic(t);
        m_labels[i]->transform.position = {
            m_labelOrigin[i].x + nudge, m_labelOrigin[i].y, m_labelOrigin[i].z,
        };
        const ::fbzz::math::Vector4 c = textfx::Mix(labelDimColor, labelActiveColor, t);
        // 文字の色は文字列側 (UiTextFx) が持ち、UIText.color は α だけにする。
        std::string rich;
        if (reveal < 1.0f) {
            // 出現は解読。乱数は 1/30 秒ごとに更新 (毎フレームだと «ノイズ» に見える)。
            const ::fbzz::math::Vector4 scramble = textfx::Mix(labelDimColor, { 0.0f, 0.0f, 0.0f, 1.0f }, 0.35f);
            rich = textfx::Decode(m_text[i], reveal, 0x51ED27u + static_cast<std::uint32_t>(i),
                                  static_cast<std::uint32_t>(m_clock * 30.0f), labelDimColor,
                                  { 1.0f, 1.0f, 1.0f, 1.0f }, scramble);
        } else if (t > 0.01f) {
            // 点いている間は 1.6 秒に 1 本、光が左から右へ字面を舐める。
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
    // 実時間で寄せる。タイトルはヒットストップやスローの影響を受けない。
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);
    m_clock += dt;
    m_intro += dt;
    debugIntro = m_intro;

    // 決定の光は扉が閉まる間も減衰させる (押した行が光ったまま固まらない)。
    for (int i = 0; i < kCount; ++i) uimotion::Decay(m_flash[i], dt, 0.16f);

    // 扉 (ワイプ)。塗っている / 剥がしている最中は入力を受けない。
    // 見た目だけは書き続ける (帯の斜線が止まると «固まった» に見える)。
    const bool busy = transition::Drive(dt, scene, postprocess, m_wipeWriting, false);

    const float attack  = blendSeconds   <= 0.0f ? 0.0f : blendSeconds;
    const float release = releaseSeconds <= 0.0f ? attack : releaseSeconds;

    debugHovered = "-";
    for (int i = 0; i < kCount; ++i) {
        const float reveal = uimotion::Stagger(m_intro - introDelay, i, introStagger, introSeconds);
        // 置き切る前の行はカーソルを受けない。滑り込んでいる最中に光ると、
        // 出現と点灯が混ざって «何が起きたか» が読めない。
        const bool ready = reveal >= 1.0f && !busy;

        // 押下中も点灯を維持する。押した瞬間に消えると、押せたのかどうかが判らない。
        const bool lit = ready && m_rows[i] && (ui.IsHovered(m_rows[i]) || ui.IsPressed(m_rows[i]));
        if (lit) debugHovered = kNames[i];
        if (lit && !m_lit[i]) {
            // 乗った瞬間。帯が開くのと同時に、小さく光って音を出す。
            audio.PlayOneShot(uinav::kMove);
            m_flash[i] = (std::max)(m_flash[i], 0.28f);
        }
        m_lit[i] = lit;
        m_litSince[i] = lit ? m_litSince[i] + dt : 0.0f;

        m_amount[i] = uimotion::Approach(m_amount[i], lit ? 1.0f : 0.0f, dt, lit ? attack : release);
        Paint(i, reveal);

        if (ready && ui.WasClicked(m_rows[i])) {
            m_flash[i] = 1.0f;
            Paint(i, reveal);   // 光った状態を今フレームに出してから扉を開ける
            Submit(i);
            return;
        }
    }
}

inline void TitleMenuComponent::Submit(int index)
{
    // EXIT だけシーンではなくアプリの終了。
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
