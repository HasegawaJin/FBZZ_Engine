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
#pragma once

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/UI/StageProgressState.hpp>
#include <Scripts/UI/UiNavSe.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

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

    FBZZ_GROUP("Flow")
    // WHY PLAY のシーン名を持たないか: 行ごとに違う。どのシーンを読むかは
    //     StageCatalog.hpp の台帳が持ち、ここは «選んだ行» を渡すだけにする。
    FBZZ_FIELD(std::string, titleScene, "Title", "CANCEL")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(std::string, debugCursor, "0", "Cursor")

    void OnStart() override;
    void OnUpdate() override;

private:
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
};

FBZZ_REFLECT(StageSelectComponent)

inline void StageSelectComponent::OnStart()
{
    StageProgressState::EnsureInit();
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
    Refresh();
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
        if (GameObject* t = ui.Find(row, "Name")) {
            ui.SetText(t, open ? StageAt(i).name : "????");
            ui.SetTextColor(t, sel ? (open ? ::fbzz::math::Vector4{ 1.0f, 1.0f, 1.0f, 1.0f }
                                           : ::fbzz::math::Vector4{ 0.490f, 0.478f, 0.459f, 1.0f })
                                   : (open ? ::fbzz::math::Vector4{ 0.463f, 0.455f, 0.439f, 1.0f }
                                           : ::fbzz::math::Vector4{ 0.247f, 0.239f, 0.227f, 1.0f }));
        }
        if (GameObject* t = ui.Find(row, "Rank"))
            ui.SetText(t, r.cleared ? StageProgressState::RankLabel(r.bestScore) : "");
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
    Text("Pane_Name", open ? info.name : "????");

    // 記録は «クリアしたことがある» ときだけ数字になる。
    const auto clock = [](float s) {
        const int t = static_cast<int>(std::round((std::max)(s, 0.0f)));
        return std::string(t / 60 < 10 ? "0" : "") + std::to_string(t / 60) + ":" +
               (t % 60 < 10 ? "0" : "") + std::to_string(t % 60);
    };
    const std::string val[3] = {
        r.cleared ? clock(r.bestSeconds) : "--",
        r.cleared ? std::to_string(r.bestChain) + " 体" : "--",
        r.cleared ? std::to_string(r.bestPush) + " 体"  : "--",
    };
    for (int i = 0; i < 3; ++i) {
        const std::string p = "Pane_Row" + std::to_string(i) + "_";
        Text(p + "Value", val[i]);
        Text(p + "Best",  val[i]);
        for (int k = 0; k < 3; ++k) {
            if (GameObject* dot = N(p + "Dot" + std::to_string(k)))
                ui.SetTextColor(dot, ::fbzz::math::Vector4{ 0.169f, 0.180f, 0.200f, 1.0f });
        }
    }
    Text("Pane_Rank_Letter", r.cleared ? StageProgressState::RankLabel(r.bestScore) : "–");
    Text("Pane_Rank_Pts",    r.cleared ? std::to_string(r.bestScore) + " / 9" : "– / 9");

    // ボスの構成は初回クリアまで伏せる。選択画面を «予習» ではなく «記録» にする。
    static constexpr const char* kBossKey[5] = { "BOSS", "HP", "PHASE 1", "PHASE 2", "SOLUTION" };
    const char* bossVal[5] = { info.boss, info.hp, info.phase1, info.phase2, info.solution };
    const char* bossSub[5] = { "", "", info.phase1Sub, info.phase2Sub, "" };
    for (int i = 0; i < 5; ++i) {
        const std::string p = "Pane_Boss" + std::to_string(i) + "_";
        Text(p + "Key",   r.cleared ? kBossKey[i] : "");
        Text(p + "Value", r.cleared ? bossVal[i] : (i == 0 ? "初回プレイ後に表示される" : ""));
        Text(p + "Sub",   r.cleared ? bossSub[i] : "");
    }
    // «解放されているのに実体が無い» 枠は、その 1 行で言い切る ─ 押しても何も
    // 起きない行に «未プレイ» と出ると、こちらの不具合に見える。
    Text("Pane_Lbl_BossSub",
         r.cleared            ? "撃破でステージクリア"
         : !StageExists(m_cursor) ? "準備中"
         : r.unlocked         ? "未プレイ"
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
    if (!scene.LoadScene(target))
        debug.LogError("StageSelect: シーンを読み込めません -> " + target +
                       " (StageCatalog.hpp の scene 名と Assets/Scenes/ を突き合わせること)");
}

inline void StageSelectComponent::OnUpdate()
{
    const float dt = (std::max)(time.UnscaledDeltaTime(), 0.0f);

    // ---- カーソル（マウス / パッド）----
    // 行そのものが UIButton なので、芯の 4px や文字の字面ではなく行全体で拾える。
    int hovered = -1;
    for (int i = 0; i < kRows; ++i)
        if (m_row[i] && (ui.IsHovered(m_row[i]) || ui.IsPressed(m_row[i]))) hovered = i;
    if (hovered >= 0 && hovered != m_cursor) {
        m_cursor = hovered;
        audio.PlayOneShot(uinav::kMove);
        Refresh();
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
    }

    if (input.GetActionDown("Submit")) {
        Play(m_cursor);
    } else if (input.GetActionDown("Cancel")) {
        audio.PlayOneShot(uinav::kCancel);
        if (!scene.LoadScene(titleScene))
            debug.LogError("StageSelect: シーンを読み込めません -> " + titleScene);
    }
}

} // namespace sandbox
