/// @file    ViewportStatsOverlay.cpp
/// @brief   Game ビューポート左下へ重ねるフレーム統計 HUD
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// @note 役割は「絵を見ながら横目で確認する層」に限定する。内訳の一覧は Analysis > Rendering が
///       表と GPU パス履歴で持つため、同じ数字を並べ直さない。ここは「playing 中に視線を
///       外さず追う値」だけに絞る。
#include "ViewportCommon.hpp"
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/FrameTimeGraph.hpp>

namespace fbzz::editor {

namespace {

/// カリング要因の識別色。内訳の対応は Analysis 側で読む前提で、ここでは割合だけ見せる。
ImU32 CullColor(int index)
{
    switch (index) {
    case 0:  return EditorTheme::ColorU32(ThemeColor::Accent);
    case 1:  return EditorTheme::ColorU32(ThemeColor::Secondary);
    case 2:  return EditorTheme::ColorU32(ThemeColor::Info, 0.75f);
    default: return EditorTheme::ColorU32(ThemeColor::Warning);
    }
}

/// 桁が伸びても値の列幅が暴れないよう K / M へ丸める。
void FormatCount(char* buf, size_t size, uint64_t value)
{
    if (value >= 1000000)
        std::snprintf(buf, size, "%.2fM", static_cast<double>(value) / 1000000.0);
    else if (value >= 10000)
        std::snprintf(buf, size, "%.1fK", static_cast<double>(value) / 1000.0);
    else
        std::snprintf(buf, size, "%llu", static_cast<unsigned long long>(value));
}

/// @brief 1 行 = 左にラベル / 右端へ値 (+ 淡い単位)。
/// @note 旧実装はラベルを空白で埋めて桁を揃えていたが、UI フォントはプロポーショナルの Roboto
///       なので値の頭が行ごとにずれる。右端で揃えれば桁数が変わっても数字の列が動かない。
void StatRow(const char* label, const char* value, const ImVec4& valueColor, const char* unit = nullptr)
{
    const float fontH  = ImGui::GetFontSize();
    const float gap    = fontH * 0.3f;
    const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;

    ImGui::TextColored(EditorTheme::Color(ThemeColor::TextMuted), "%s", label);

    const float unitW = unit ? ImGui::CalcTextSize(unit).x + gap : 0.0f;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(rightX - ImGui::CalcTextSize(value).x - unitW);
    ImGui::TextColored(valueColor, "%s", value);
    if (unit) {
        ImGui::SameLine(0.0f, gap);
        ImGui::TextColored(EditorTheme::Color(ThemeColor::TextFaint), "%s", unit);
    }
}

/// カリング内訳の積み上げバー。残りが実際に描いた分。
void CullBar(const int* culled, int count, int total, float height)
{
    const float  width  = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({ width, height });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, { origin.x + width, origin.y + height },
                      EditorTheme::ColorU32(ThemeColor::Field, 0.8f), 2.0f);
    if (total <= 0) return;

    float x = origin.x;
    for (int i = 0; i < count; ++i) {
        const float w = width * static_cast<float>(culled[i]) / static_cast<float>(total);
        if (w <= 0.0f) continue;
        dl->AddRectFilled({ x, origin.y }, { x + w, origin.y + height }, CullColor(i));
        x += w;
    }
    /// @note 描画された分は溝の色のまま残し、境界だけ立てて「ここから先が実描画」を示す。
    if (x > origin.x + 0.5f)
        dl->AddLine({ x, origin.y }, { x, origin.y + height },
                    EditorTheme::ColorU32(ThemeColor::Canvas), 1.0f);
}

float SumMilliseconds(const std::vector<std::pair<std::string, double>>& timings)
{
    double total = 0.0;
    for (const auto& entry : timings)
        total += entry.second;
    return static_cast<float>(total);
}

} // namespace

void DrawStatsOverlay(EditorContext& ctx)
{
    const auto& snapshot = renderer::RenderDebugOverlay::GetLastSnapshot();
    const auto& rs       = snapshot.renderStats;

    /// @note 目標フレーム時間を「予算」として扱う。色分けもグラフの基準線もこれ 1 本に揃える。
    const int   targetFps = ctx.projectSettings.app.targetFps > 0 ? ctx.projectSettings.app.targetFps : 60;
    const float targetMs  = 1000.0f / static_cast<float>(targetFps);

    /// @note 左下に配置 (タブバー・ツールバーと重ならないよう上マージンを考慮)。右上は ImGuizmo の
    ///       ビューキューブと重なりやすく、左下はほぼ空きスペースになるため視認性が高い。
    const float  fontH   = ImGui::GetFontSize();
    const float  margin  = fontH * 0.7f;
    const ImVec2 winPos  = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    ImGui::SetNextWindowPos({ winPos.x + margin, winPos.y + winSize.y - margin },
                            /// @note pivot: 左下
                            ImGuiCond_Always, { 0.0f, 1.0f });
    ImGui::SetNextWindowSize({ fontH * 14.5f, 0.0f }, ImGuiCond_Always);

    ImVec4 panelBg = EditorTheme::Color(ThemeColor::Surface);
    panelBg.w      = 0.85f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, panelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, EditorTheme::Color(ThemeColor::Border));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { fontH * 0.8f, fontH * 0.6f });
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 0.0f, fontH * 0.18f });

    constexpr ImGuiWindowFlags kOverlayFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin("##vp_stats", nullptr, kOverlayFlags)) {
        char buf[32];
        char value[48];

        /// @note フレーム時間は Analysis > Rendering と同じ部品・同じ履歴で描く。
        widgets::FrameTimeHero(targetMs);
        widgets::FrameTimeGraph(targetMs, fontH * 1.7f);

        /// @note GPU 時間だけは内訳を開かずに見たい。CPU 待ちか GPU 律速かの一次切り分けになる。
        if (const float gpuMs = SumMilliseconds(snapshot.gpuPassTimings); gpuMs > 0.0f) {
            std::snprintf(value, sizeof(value), "%.2f", gpuMs);
            StatRow("GPU", value, EditorTheme::Color(ThemeColor::Text), "ms");
        }

        /// @note 視点を動かすたびに動く 3 つ。ここだけは絵を見ながら追う価値がある。
        std::snprintf(value, sizeof(value), "%d", rs.drawCalls);
        StatRow("Draw calls", value, EditorTheme::Color(ThemeColor::Text));
        FormatCount(value, sizeof(value), static_cast<uint64_t>(std::max(rs.triangleCount, 0)));
        StatRow("Triangles", value, EditorTheme::Color(ThemeColor::Text));
        FormatCount(value, sizeof(value), static_cast<uint64_t>(std::max(rs.vertexCount, 0)));
        StatRow("Vertices", value, EditorTheme::Color(ThemeColor::Text));

        /// @name カリング (率のみ)
        /// @note 要因別の件数は Analysis > Rendering の表で読む。ここでは効いているかだけ見る。
        const int culled[4] = { rs.frustumCulled, rs.occlusionCulled,
                                rs.distanceCulled, rs.smallObjectCulled };
        const int totalCulled = culled[0] + culled[1] + culled[2] + culled[3];
        const float cullRate  = rs.totalObjects > 0
            ? static_cast<float>(totalCulled) / static_cast<float>(rs.totalObjects) * 100.0f
            : 0.0f;

        std::snprintf(value, sizeof(value), "%.0f%%", cullRate);
        std::snprintf(buf, sizeof(buf), "of %d", rs.totalObjects);
        StatRow("Culled", value,
                (cullRate >= 50.0f) ? EditorTheme::Color(ThemeColor::Success)
              : (cullRate >= 30.0f) ? EditorTheme::Color(ThemeColor::Warning)
                                    : EditorTheme::Color(ThemeColor::TextMuted),
                buf);
        CullBar(culled, 4, rs.totalObjects, fontH * 0.4f);
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

} // namespace fbzz::editor
