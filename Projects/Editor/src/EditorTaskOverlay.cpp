// FBZZ Engine
// EditorTaskOverlay.cpp | fbzz::editor
// モーダルオーバーレイの描画と状態管理
#include <Editor/EditorTaskOverlay.hpp>
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fbzz::editor {

bool  EditorTaskOverlay::s_active   = false;
bool  EditorTaskOverlay::s_needOpen = false;
char  EditorTaskOverlay::s_taskName[128] = {};
char  EditorTaskOverlay::s_status[256]   = {};
float EditorTaskOverlay::s_progress      = -1.0f;

void EditorTaskOverlay::Begin(const char* taskName)
{
    s_active   = true;
    s_needOpen = true;
    std::strncpy(s_taskName, taskName ? taskName : "", sizeof(s_taskName) - 1);
    s_taskName[sizeof(s_taskName) - 1] = '\0';
    s_status[0] = '\0';
    s_progress  = -1.0f;
}

void EditorTaskOverlay::SetStatus(const char* status)
{
    std::strncpy(s_status, status ? status : "", sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

void EditorTaskOverlay::SetProgress(float progress)
{
    s_progress = progress;
}

void EditorTaskOverlay::End()
{
    s_active = false;
    // ImGui::CloseCurrentPopup は Render() 内で処理する
}

void EditorTaskOverlay::Render()
{
    // BeginPopupModal は OpenPopup を呼んだフレームと同じフレームで開く必要がある
    if (s_needOpen)
    {
        ImGui::OpenPopup("##EditorTaskOverlay");
        s_needOpen = false;
    }

    // ポップアップが開いていない かつ 非アクティブなら何もしない
    if (!ImGui::IsPopupOpen("##EditorTaskOverlay"))
        return;

    // 背景を暗く塗りつぶす
    // WHY: BeginPopupModal のデフォルト暗幕は DimBgFactor で制御できるが
    //      エディターテーマ依存になるため、ForegroundDrawList で確実に描画する。
    {
        const ImVec2 dmax = ImGui::GetIO().DisplaySize;
        ImGui::GetForegroundDrawList()->AddRectFilled(
            {0.0f, 0.0f}, dmax, IM_COL32(0, 0, 0, 160));
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({380.0f, 0.0f}, ImGuiCond_Always);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration  |
        ImGuiWindowFlags_NoMove        |
        ImGuiWindowFlags_NoScrollbar   |
        ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginPopupModal("##EditorTaskOverlay", nullptr, kFlags))
    {
        // タイトル
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
        ImGui::TextUnformatted(s_taskName);
        ImGui::PopStyleColor();

        ImGui::Separator();
        ImGui::Spacing();

        // ステータステキスト
        if (s_status[0] != '\0')
            ImGui::TextUnformatted(s_status);
        else
            ImGui::TextDisabled("処理中...");

        ImGui::Spacing();

        // プログレスバー
        // s_progress < 0 のとき: 時間に基づいてループするアニメーションを表示
        if (s_progress >= 0.0f)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f%%", s_progress * 100.0f);
            ImGui::ProgressBar(s_progress, {-1.0f, 0.0f}, buf);
        }
        else
        {
            // インジケーター: 0→1→0 をループ
            const float t    = std::fmod(static_cast<float>(ImGui::GetTime()) * 1.2f, 2.0f);
            const float frac = t < 1.0f ? t : 2.0f - t;
            ImGui::ProgressBar(frac, {-1.0f, 0.0f}, "");
        }

        ImGui::Spacing();

        // 処理完了したらここでポップアップを閉じる
        if (!s_active)
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

} // namespace fbzz::editor
