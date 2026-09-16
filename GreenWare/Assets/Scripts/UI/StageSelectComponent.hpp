/// @file    StageSelectComponent.hpp
/// @brief   StageSelect.scene。行の選択と、右パネルの記録表示。
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// 画面の作り（Assets/UI/README_Result_Select.md と同じ）:
///   Canvas
///     Head       ─ ＋−マーク / 罫線 / レール / 位置表示
///     Rows       ─ Row0..6（Bar_on / Bar_dim / Bar_lock / Chip / No / Name / Rank）
///     Dividers   ─ 区切り線
///     Pane       ─ 右の記録とボスの構成
///     ListFade   ─ 一覧の下端のフェード（«続きがある» の表示）
///
/// WHY 行を可変にするか:
///   ステージ数は未定。行を 76px ピッチで並べ、レールのつまみの長さを
///   «可視 / 全体» で決めておけば、Row を増やすだけで器が伸びる。
///
/// WHY 未解放の行にもカーソルが乗るか:
///   解放条件を読ませたい。ただし選べるようには見せないので、
///   選択中でも Bar_lock（赤くない芯）を出す。
///
/// WHY カーソルを動かすたびに右パネルを «置き直す» か:
///   パネルの文字だけが書き換わると、記録が «同じ紙の上で数字が変わった» ように
///   見えて、別のステージを見ていることが伝わらない。右から数 px 滑り込みつつ
///   浮かび上がると、«次の紙が来た» と読める。距離は短く、時間も 0.2 秒前後 ─
///   長いと上下キーを連打したときに追いつかず、パネルが常に薄い。
#pragma once

#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/UI/StageProgressState.hpp>
#include <Scripts/UI/UiMotion.hpp>
#include <Scripts/UI/UiNavSe.hpp>
#include <Scripts/UI/UiTextFx.hpp>
#include <Scripts/Utils/BgmLibrary.hpp>
#include <Scripts/Utils/SceneTransition.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace fbzz::scene;


namespace sandbox {

class StageSelectComponent : public Script {
    FBZZ_SCRIPT(StageSelectComponent)

public:
    FBZZ_GROUP("List")
    FBZZ_FIELD(float, rowPitch,    76.0f, "Row Pitch")
    FBZZ_TOOLTIP("行の間隔。シーン側の Row の並びと同じ値にすること")
    FBZZ_FIELD(int,   visibleRows, 6,     "Visible Rows")
    FBZZ_TOOLTIP("一覧に入る行数。レールのつまみの長さをこれで決める")

    FBZZ_GROUP("導入")
    FBZZ_FIELD_RANGE(float, introDelay, 0.30f, "遅延", 0.0f, 3.0f)
    FBZZ_TOOLTIP("画面に入ってから 1 行目が動き始めるまで。題字の出現 (UiReveal) の後に")
    FBZZ_FIELD_RANGE(float, introStagger, 0.05f, "のけぞり", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE(float, introSeconds, 0.40f, "継続時間", 0.05f, 3.0f)
    FBZZ_FIELD(float, introSlide, -36.0f, "滑り")
    FBZZ_TOOLTIP("行が出る前の位置のずれ [px]。負で左から滑り込む")

    FBZZ_GROUP("Pane")
    FBZZ_FIELD(float, paneSlide, 18.0f, "滑り")
    FBZZ_TOOLTIP("選択が変わったとき、パネルが出る前の位置のずれ [px]。正で右から")
    FBZZ_FIELD_RANGE(float, paneSeconds, 0.22f, "継続時間", 0.05f, 2.0f)
    FBZZ_FIELD_RANGE(float, paneStagger, 0.008f, "のけぞり", 0.0f, 0.2f)
    FBZZ_FIELD(float, nameNudge, 8.0f, "Name Nudge")
    FBZZ_TOOLTIP("選択中の行の名前を右へ押し出す量 [px]")

    FBZZ_GROUP("流れ")
    // WHY PLAY のシーン名を持たないか: 行ごとに違う。どのシーンを読むかは
    //     StageCatalog.hpp の台帳が持ち、ここは «選んだ行» を渡すだけにする。
    FBZZ_FIELD(std::string, titleScene, "Title", "キャンセル")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(std::string, debugCursor, "0", "カーソル")

    void OnStart() override;
    void OnUpdate() override;

private:
    bool m_wipeWriting = false;   ///< 扉を自分で postprocess へ書いたか (transition::Drive)
    static constexpr int kRows = StageProgressState::kCount;
    /// «選べる» 行か。解放されていて、かつシーンの実体があること。
    [[nodiscard]] static bool Selectable(int index)
    {
        return index >= 0 && index < kRows && StageProgressState::stages[index].unlocked
            && StageExists(index);
    }

    [[nodiscard]] GameObject* N(std::string_view name) const { return scene.Find(std::string(name)); }
    void Text(std::string_view name, const std::string& v) const
    {
        if (GameObject* go = N(name)) ui.SetText(go, v);
    }
    void Refresh();
    void RefreshPane();
    void Play(int index);

    GameObject* m_row[kRows] = {};
    int   m_cursor = 0;
    float m_repeat = 0.0f;

    /// 行の出現。行そのものの位置と、子の «置いてあった色» を控える。
    ::fbzz::math::Vector3        m_rowOrigin[kRows]  = {};
    ::fbzz::math::Vector3        m_nameOrigin[kRows] = {};
    ::fbzz::math::Vector4        m_nameColor[kRows]  = {};   ///< Refresh が決めた名前の色
    std::vector<uimotion::Slot>  m_rowParts[kRows];          ///< Name 以外の子
    float m_nudge[kRows] = {};                                ///< 名前の寄り 0..1
    float m_intro = 0.0f;
    std::string m_nameText[kRows];   ///< 行の名前の素の文言 (Refresh が決める)
    std::string m_nameRich[kRows];   ///< 最後に流し込んだ文字列
    float m_selSince = 0.0f;         ///< 選択が動いてからの秒数 (名前の走査)
    std::string m_paneNameText;      ///< パネルの見出しの素の文言
    std::string m_paneNameRich;

    /// 右パネル。子を全部控え、選択が変わるたびに置き直す。
    std::vector<uimotion::Slot> m_pane;
    float m_paneClock = 1.0e3f;   ///< 置き直しを始めてからの秒数。大きい = 済んでいる

    void PaintRows(float dt);
    void PaintPane();
    void RestartPane() { m_paneClock = 0.0f; m_selSince = 0.0f; PaintPane(); }
};

FBZZ_REFLECT(StageSelectComponent)

inline void StageSelectComponent::OnStart()
{
    // 解放と自己ベストはここでディスクから読む (起動して最初に触る画面なので)。
    StageProgressState::EnsureInit(save);
    for (int i = 0; i < kRows; ++i) {
        m_row[i] = N("Row" + std::to_string(i));
        if (!m_row[i]) debug.LogWarning("StageSelect: Row" + std::to_string(i) + " が無い");
    }
    m_cursor = (std::clamp)(StageProgressState::cursor, 0, kRows - 1);

    // つまみの長さは «入る行数 / 全体». 行が増えても式は変わらない。
    if (GameObject* thumb = N("Rail_Thumb")) {
        const float visible = rowPitch * static_cast<float>(visibleRows);
        const float total   = rowPitch * static_cast<float>(kRows);
        const float railH   = 524.0f;    // レールの実寸（Reference と同じ）
        const float len     = railH * (std::min)(1.0f, visible / (std::max)(total, 1.0f));
        thumb->transform.scale.y = len + 96.0f;   // PNG は上下 48px ずつ余白を持つ
    }
    se::EnsureSource(scene, "UI");
    bgm::Play(audio, bgm::kStageSelect);
    Refresh();

    // 出現のために «置いてあった位置と色» を控える。Refresh の後で控えるのは、
    // 名前と番号の色は Refresh が決めるので、その結果を基準にしたいため。
    for (int i = 0; i < kRows; ++i) {
        GameObject* row = m_row[i];
        if (!row) continue;
        m_rowOrigin[i] = row->transform.position;
        m_rowParts[i].clear();
        for (int c = 0, n = row->GetChildCount(); c < n; ++c) {
            GameObject* child = row->GetChild(c);
            if (!child) continue;
            if (child->name == "Name") {
                m_nameOrigin[i] = child->transform.position;
                if (auto* t = child->GetComponent<UIText>()) t->richText = true;   // 色付き文字列を流す
                continue;
            }
            uimotion::Slot slot;
            slot.go     = child;
            slot.origin = child->transform.position;
            slot.image  = child->GetComponent<UIImage>() != nullptr;
            slot.text   = child->GetComponent<UIText>() != nullptr;
            if (slot.image)     slot.color = ui.GetImageColor(child);
            else if (slot.text) slot.color = ui.GetTextColor(child);
            m_rowParts[i].push_back(slot);
        }
    }
    if (GameObject* name = N("Pane_Name"))
        if (auto* t = name->GetComponent<UIText>()) t->richText = true;
    if (GameObject* pane = N("Pane")) {
        m_pane.clear();
        for (int c = 0, n = pane->GetChildCount(); c < n; ++c) {
            GameObject* child = pane->GetChild(c);
            if (!child) continue;
            uimotion::Slot slot;
            slot.go     = child;
            slot.origin = child->transform.position;
            slot.image  = child->GetComponent<UIImage>() != nullptr;
            slot.text   = child->GetComponent<UIText>() != nullptr;
            if (slot.image)     slot.color = ui.GetImageColor(child);
            else if (slot.text) slot.color = ui.GetTextColor(child);
            m_pane.push_back(slot);
        }
    }
    m_intro = 0.0f;
    // 1 フレーム目から «出ていない» で描く。
    PaintRows(0.0f);
    m_paneClock = -introDelay;   // パネルも行と同じ拍で最初の 1 回を出す
    PaintPane();
}

inline void StageSelectComponent::PaintRows(float dt)
{
    for (int i = 0; i < kRows; ++i) {
        GameObject* row = m_row[i];
        if (!row) continue;
        const float t     = uimotion::Stagger(m_intro - introDelay, i, introStagger, introSeconds);
        const float e     = uimotion::OutCubic(t);
        const float alpha = uimotion::OutQuint(t * 1.25f);
        row->transform.position = { m_rowOrigin[i].x + introSlide * (1.0f - e),
                                    m_rowOrigin[i].y, m_rowOrigin[i].z };
        for (uimotion::Slot& part : m_rowParts[i]) {
            if (!part.go) continue;
            const ::fbzz::math::Vector4 c = { part.color.x, part.color.y, part.color.z,
                                              part.color.w * alpha };
            if (part.image)     ui.SetImageColor(part.go, c);
            else if (part.text) ui.SetTextColor(part.go, c);
        }
        // 選択中の行は名前が芯から押し出される。戻りは寄りより遅く。
        const bool sel = (i == m_cursor);
        m_nudge[i] = uimotion::Approach(m_nudge[i], sel ? 1.0f : 0.0f, dt, sel ? 0.08f : 0.16f);
        if (GameObject* name = ui.Find(row, "Name")) {
            name->transform.position = { m_nameOrigin[i].x + nameNudge * uimotion::OutCubic(m_nudge[i]),
                                         m_nameOrigin[i].y, m_nameOrigin[i].z };
            // 出現は解読、選ばれた直後は走査 1 本、それ以外は 1 色。
            std::string rich;
            const std::uint32_t tick = static_cast<std::uint32_t>(m_intro * 30.0f);
            if (t < 1.0f) {
                rich = textfx::Decode(m_nameText[i], t, 0x3A7Fu + static_cast<std::uint32_t>(i), tick,
                                      m_nameColor[i], { 1.0f, 1.0f, 1.0f, 1.0f },
                                      textfx::Mix(m_nameColor[i], { 0.0f, 0.0f, 0.0f, 1.0f }, 0.35f));
            } else if (sel && m_selSince < 0.55f) {
                rich = textfx::Sweep(m_nameText[i], m_nameColor[i], { 1.0f, 1.0f, 1.0f, 1.0f },
                                     -0.4f + 1.8f * (m_selSince / 0.55f), 0.35f);
            } else {
                rich = textfx::Wrap(m_nameText[i], m_nameColor[i]);
            }
            if (rich != m_nameRich[i]) { ui.SetText(name, rich); m_nameRich[i] = std::move(rich); }
            ui.SetTextColor(name, { 1.0f, 1.0f, 1.0f, alpha });
        }
    }
}

inline void StageSelectComponent::PaintPane()
{
    // 見出しは解読で出す。他の子は位置と α だけ (数字まで乱すと記録が読めない)。
    if (GameObject* name = N("Pane_Name")) {
        const float p = std::clamp(m_paneClock / (std::max)(paneSeconds * 1.4f, 0.05f), 0.0f, 1.0f);
        const ::fbzz::math::Vector4 base = { 0.949f, 0.941f, 0.925f, 1.0f };
        std::string rich = textfx::Decode(m_paneNameText, p, 0xC0FFEEu + static_cast<std::uint32_t>(m_cursor),
                                          static_cast<std::uint32_t>(m_paneClock * 30.0f), base,
                                          { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.45f, 0.46f, 0.50f, 0.9f });
        if (rich != m_paneNameRich) { ui.SetText(name, rich); m_paneNameRich = std::move(rich); }
    }
    for (std::size_t i = 0; i < m_pane.size(); ++i) {
        uimotion::Slot& slot = m_pane[i];
        if (!slot.go) continue;
        const float t = uimotion::Stagger(m_paneClock, static_cast<int>(i), paneStagger, paneSeconds);
        const ::fbzz::math::Vector4 c =
            uimotion::Place(slot, t, { paneSlide, 0.0f, 0.0f }, uimotion::OutQuint(t * 1.25f));
        if (slot.image)     ui.SetImageColor(slot.go, c);
        else if (slot.text) ui.SetTextColor(slot.go, c);
    }
}

inline void StageSelectComponent::Refresh()
{
    for (int i = 0; i < kRows; ++i) {
        GameObject* row = m_row[i];
        if (!row) continue;
        const StageRecord& r = StageProgressState::stages[i];
        const bool sel  = (i == m_cursor);
        const bool open = Selectable(i);

        // 芯は 3 枚。選択中でも、選べない行なら赤くしない。
        if (GameObject* b = ui.Find(row, "Bar_on"))   b->SetActive(sel && open);
        if (GameObject* b = ui.Find(row, "Bar_lock")) b->SetActive(sel && !open);
        if (GameObject* b = ui.Find(row, "Bar_dim"))  b->SetActive(!sel);
        // NEW は «選べて、まだクリアしていない» 行だけ。
        if (GameObject* c = ui.Find(row, "Chip")) c->SetActive(open && !r.cleared);

        char no[8] = {};
        std::snprintf(no, sizeof(no), "%02d", i + 1);
        if (GameObject* t = ui.Find(row, "No"))   ui.SetText(t, no);
        // 名前は文言も色も控えるだけ。書くのは PaintRows (解読 / 走査 / α をまとめて書く)。
        {
            m_nameText[i] = open ? StageAt(i).name : "????";
            m_nameColor[i] = sel ? (open ? ::fbzz::math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }
                                         : ::fbzz::math::Vector4{ 0.490f, 0.478f, 0.459f, 1.0f })
                                 : (open ? ::fbzz::math::Vector4{ 0.463f, 0.455f, 0.439f, 1.0f }
                                         : ::fbzz::math::Vector4{ 0.247f, 0.239f, 0.227f, 1.0f });
        }
        if (GameObject* t = ui.Find(row, "Rank"))
            ui.SetText(t, r.HasCurrentScore() ? StageProgressState::RankLabel(r.bestScore) : "");
    }
    char cnt[16] = {};
    std::snprintf(cnt, sizeof(cnt), "%02d / %02d", m_cursor + 1, kRows);
    Text("Count", cnt);
    debugCursor = std::to_string(m_cursor);
    RefreshPane();
}

inline void StageSelectComponent::RefreshPane()
{
    const StageRecord& r    = StageProgressState::stages[m_cursor];
    const StageEntry&  info = StageAt(m_cursor);
    const bool         open = Selectable(m_cursor);

    char no[16] = {};
    std::snprintf(no, sizeof(no), "STAGE %02d", m_cursor + 1);
    Text("Pane_Eyebrow", no);
    m_paneNameText = open ? info.name : "????";

    // 記録は «クリアしたことがある» ときだけ数字になる。
    const auto clock = [](float s) {
        const int t = static_cast<int>(std::ceil((std::max)(s, 0.0f)));
        return std::string(t / 60 < 10 ? "0" : "") + std::to_string(t / 60) + ":" +
               (t % 60 < 10 ? "0" : "") + std::to_string(t % 60);
    };
    const std::string val[3] = {
        r.HasCurrentScore() ? clock(r.bestSeconds) : "--",
        r.HasCurrentScore() ? GameResultState::TechniqueText(r.bestTechnique) : "--",
        r.HasCurrentScore() && r.leastDamage >= 0 ? GameResultState::DamageText(r.leastDamage) : "--",
    };
    for (int i = 0; i < 3; ++i) {
        const std::string p = "Pane_Row" + std::to_string(i) + "_";
        // 見出しと達成ラインは採点表から引く。リザルトと別の文字を持つと片方だけ古くなる。
        Text(p + "Name", GameResultState::Axis(m_cursor, i).name);
        Text(p + "Th",   GameResultState::Axis(m_cursor, i).thresholds);
        Text(p + "Value", val[i]);
        Text(p + "Best",  val[i]);
        for (int k = 0; k < 3; ++k) {
            if (GameObject* dot = N(p + "Dot" + std::to_string(k)))
                ui.SetTextColor(dot, ::fbzz::math::Vector4{ 0.169f, 0.180f, 0.200f, 1.0f });
        }
    }
    Text("Pane_Rank_Letter", r.HasCurrentScore() ? StageProgressState::RankLabel(r.bestScore) : "–");
    Text("Pane_Rank_Pts", r.HasCurrentScore() ? std::to_string(r.bestScore) + " / 9"
                                             : r.cleared ? "新基準で未記録" : "– / 9");

    // ボスの構成は初回クリアまで伏せる。選択画面を «予習» ではなく «記録» にする。
    // 3 段階のボスとチュートリアルを同じ欄へ収めるため、段階番号を見出しにしない。
    static constexpr const char* kBossKey[5] = { "ボス", "部位", "戦い方", "変化", "攻略" };
    const char* bossVal[5] = { info.boss, info.parts, info.phase1, info.phase2, info.solution };
    const char* bossSub[5] = { "", "", info.phase1Sub, info.phase2Sub, "" };
    for (int i = 0; i < 5; ++i) {
        const std::string p = "Pane_Boss" + std::to_string(i) + "_";
        Text(p + "Key",   r.cleared ? kBossKey[i] : "");
        Text(p + "Value", r.cleared ? bossVal[i] :
             (i == 0 && StageExists(m_cursor) ? "初回クリア後に攻略情報を表示" : ""));
        Text(p + "Sub",   r.cleared ? bossSub[i] : "");
    }
    // «解放されているのに実体が無い» 枠は、その 1 行で言い切る ─ 押しても何も
    // 起きない行に «未プレイ» と出ると、こちらの不具合に見える。
    Text("Pane_Lbl_BossSub",
         r.cleared            ? "撃破でステージクリア"
         : !StageExists(m_cursor) ? "準備中"
         : r.unlocked         ? "未クリア"
                              : "前のステージをクリアすると解放される");
}

inline void StageSelectComponent::Play(int index)
{
    // 選べない行は «押せる» ように見せていないので、押しても進まない。
    if (!Selectable(index)) { audio.PlayOneShot(uinav::kCancel); return; }

    // どのステージを遊んだかは、リザルトが記録を書く先とリトライ先の両方になる。
    StageProgressState::cursor = index;
    audio.PlayOneShot(uinav::kConfirm);

    const std::string target = StageAt(index).scene;
    if (!transition::Begin(target, true))
        debug.LogError("StageSelect: シーンへの扉を開けません -> " + target +
                       " (StageCatalog.hpp の scene 名と Assets/Scenes/ を突き合わせること)");
}

inline void StageSelectComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    // 出現は扉 (ワイプ) が開いている最中も進める (止めると開き切った瞬間に飛び出す)。
    m_intro     += dt;
    m_paneClock += dt;
    m_selSince  += dt;
    PaintRows(dt);
    PaintPane();

    // 扉 (ワイプ)。塗っている / 剥がしている最中は入力を受けない。
    if (transition::Drive(dt, scene, postprocess, m_wipeWriting, false)) return;

    // ---- カーソル（マウス / パッド）----
    // 行そのものが UIButton なので、芯の 4px や文字の字面ではなく行全体で拾える。
    int hovered = -1;
    for (int i = 0; i < kRows; ++i)
        if (m_row[i] && (ui.IsHovered(m_row[i]) || ui.IsPressed(m_row[i]))) hovered = i;
    if (hovered >= 0 && hovered != m_cursor) {
        m_cursor = hovered;
        audio.PlayOneShot(uinav::kMove);
        Refresh();
        RestartPane();
    }
    for (int i = 0; i < kRows; ++i)
        if (m_row[i] && ui.WasClicked(m_row[i])) { Play(i); return; }

    // ---- キーとパッドの上下 ----
    const float v = input.GetMoveAxis().y;
    m_repeat -= dt;
    if (std::abs(v) < 0.4f) m_repeat = 0.0f;
    else if (m_repeat <= 0.0f) {
        m_cursor = (std::clamp)(m_cursor + (v < 0.0f ? 1 : -1), 0, kRows - 1);
        m_repeat = 0.20f;
        audio.PlayOneShot(uinav::kMove);
        Refresh();
        RestartPane();
    }

    if (input.GetActionDown("Submit")) {
        Play(m_cursor);
    } else if (input.GetActionDown("Cancel")) {
        audio.PlayOneShot(uinav::kCancel);
        if (!transition::Begin(titleScene))
            debug.LogError("StageSelect: シーンへの扉を開けません -> " + titleScene);
    }
}

} // namespace sandbox
