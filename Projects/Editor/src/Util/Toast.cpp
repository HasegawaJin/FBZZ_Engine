// FBZZ Engine
// Toast.cpp | fbzz::editor
// トースト通知の描画実装
#include <Editor/Util/Toast.hpp>
#include <imgui.h>
#include <algorithm>

namespace fbzz::editor {

std::vector<Toast::Entry> Toast::s_entries;

void Toast::Push(Level level, std::string message, float durationSec)
{
    // 同一メッセージが連続で積まれた場合 (例: 監視ループの重複通知) は最新へ寄せて重複を避ける。
    for (auto& e : s_entries) {
        if (e.level == level && e.message == message) {
            e.age = 0.0f;
            e.duration = durationSec;
            return;
        }
    }
    s_entries.push_back(Entry{ level, std::move(message), 0.0f, durationSec });

    // 溜まりすぎ防止 (古いものから捨てる)。
    constexpr size_t kMaxVisible = 6;
    if (s_entries.size() > kMaxVisible)
        s_entries.erase(s_entries.begin(), s_entries.begin() + (s_entries.size() - kMaxVisible));
}

namespace {

// レベルごとのアクセント色 (左端バー・アイコン)。
ImVec4 AccentColor(Toast::Level level)
{
    switch (level) {
    case Toast::Level::Success: return { 0.32f, 0.82f, 0.42f, 1.0f };
    case Toast::Level::Warning: return { 0.95f, 0.68f, 0.20f, 1.0f };
    case Toast::Level::Error:   return { 0.93f, 0.36f, 0.34f, 1.0f };
    case Toast::Level::Info:
    default:                    return { 0.35f, 0.62f, 0.95f, 1.0f };
    }
}

// レベルを示す短い記号 (フォント非依存の ASCII)。
const char* Glyph(Toast::Level level)
{
    switch (level) {
    case Toast::Level::Success: return "\xE2\x9C\x93"; // ✓
    case Toast::Level::Warning: return "!";
    case Toast::Level::Error:   return "\xC3\x97";      // ×
    case Toast::Level::Info:
    default:                    return "i";
    }
}

} // namespace

void Toast::Render()
{
    if (s_entries.empty()) return;

    const float dt = ImGui::GetIO().DeltaTime;
    for (auto& e : s_entries)
        e.age += dt;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    constexpr float kPad     = 12.0f;   // 画面端からの余白
    constexpr float kGap     = 8.0f;    // トースト間の隙間
    constexpr float kWidth   = 320.0f;
    constexpr float kFadeIn  = 0.18f;
    constexpr float kFadeOut = 0.5f;

    const float rightX  = vp->WorkPos.x + vp->WorkSize.x - kPad;
    float       bottomY = vp->WorkPos.y + vp->WorkSize.y - kPad;

    // WHY: 幅は kWidth 固定・高さは 0 指定で内容に合わせて自動調整する (AlwaysAutoResize は使わない。
    //      固定幅指定と競合してテキスト折返しが安定しないため)。
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

    // 新しい通知ほど下に積む (発生位置の近くに見える)。
    for (int i = static_cast<int>(s_entries.size()) - 1; i >= 0; --i) {
        Entry& e = s_entries[i];

        // フェード係数を計算する。
        float alpha = 1.0f;
        if (e.age < kFadeIn)
            alpha = e.age / kFadeIn;
        else if (e.age > e.duration - kFadeOut)
            alpha = std::max(0.0f, (e.duration - e.age) / kFadeOut);
        alpha = std::clamp(alpha, 0.0f, 1.0f);

        char id[32];
        std::snprintf(id, sizeof(id), "##toast_%d", i);

        ImGui::SetNextWindowBgAlpha(0.92f * alpha);
        // ピボット (1,1) = 右下基準。bottomY を下端に合わせて積み上げる。
        ImGui::SetNextWindowPos({ rightX, bottomY }, ImGuiCond_Always, { 1.0f, 1.0f });
        ImGui::SetNextWindowSize({ kWidth, 0.0f }, ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 10.0f, 8.0f });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        if (ImGui::Begin(id, nullptr, kFlags)) {
            const ImVec4 accent = AccentColor(e.level);
            const ImVec2 wmin = ImGui::GetWindowPos();
            const ImVec2 wmax = { wmin.x + ImGui::GetWindowSize().x, wmin.y + ImGui::GetWindowSize().y };

            // 左端のアクセントバー。
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec4 barCol = accent; barCol.w *= alpha;
            dl->AddRectFilled(wmin, { wmin.x + 4.0f, wmax.y },
                              ImGui::GetColorU32(barCol), 6.0f, ImDrawFlags_RoundCornersLeft);

            ImGui::Dummy({ 2.0f, 0.0f });
            ImGui::SameLine();
            ImGui::TextColored(accent, "%s", Glyph(e.level));
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::PushTextWrapPos(wmin.x + kWidth - 16.0f);
            ImGui::TextUnformatted(e.message.c_str());
            ImGui::PopTextWrapPos();

            bottomY = ImGui::GetWindowPos().y - kGap;
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    // 期限切れを除去する。
    s_entries.erase(
        std::remove_if(s_entries.begin(), s_entries.end(),
                       [](const Entry& e) { return e.age >= e.duration; }),
        s_entries.end());
}

} // namespace fbzz::editor
