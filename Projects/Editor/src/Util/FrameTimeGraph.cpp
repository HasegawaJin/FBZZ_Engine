/// @file   FrameTimeGraph.cpp
/// @brief  フレーム時間の履歴と、その大きな数値表示 / 面グラフ
/// @author Hasegawa Jin
/// @date   2026-08-24
#include <Editor/Util/FrameTimeGraph.hpp>

#include <Editor/Util/EditorTheme.hpp>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace fbzz::editor::widgets {

namespace {

constexpr int kHistory = 96;

float s_history[kHistory] = {};
int   s_head              = 0;
int   s_lastSampledFrame  = -1;

} // namespace

ImVec4 FrameBudgetColor(float ms, float targetMs)
{
    if (ms <= targetMs * 1.05f) return EditorTheme::Color(ThemeColor::Success);
    if (ms <= targetMs * 1.50f) return EditorTheme::Color(ThemeColor::Warning);
    return EditorTheme::Color(ThemeColor::Danger);
}

void SampleFrameTime()
{
    const int frame = ImGui::GetFrameCount();
    if (frame == s_lastSampledFrame) return;
    s_lastSampledFrame = frame;

    s_history[s_head] = ImGui::GetIO().DeltaTime * 1000.0f;
    s_head            = (s_head + 1) % kHistory;
}

ImVec4 FrameTimeHero(float targetMs, float width)
{
    SampleFrameTime();

    // 表示は ImGui の移動平均を使う。生の DeltaTime は毎フレーム跳ねて数字として読めない。
    const float  fps   = ImGui::GetIO().Framerate;
    const float  ms    = fps > 0.0f ? 1000.0f / fps : 0.0f;
    const ImVec4 color = FrameBudgetColor(ms, targetMs);

    char msText[32];
    char fpsText[32];
    std::snprintf(msText, sizeof(msText), "%.1f", ms);
    std::snprintf(fpsText, sizeof(fpsText), "%.0f fps", fps);

    const float  fontH    = ImGui::GetFontSize();
    const float  heroSize = fontH * 1.9f;
    const float  rowW     = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    ImFont*      font     = ImGui::GetFont();
    const ImVec2 heroDim  = font->CalcTextSizeA(heroSize, FLT_MAX, 0.0f, msText);
    const ImVec2 origin   = ImGui::GetCursorScreenPos();
    ImGui::Dummy({ rowW, heroSize });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(font, heroSize, origin, ImGui::GetColorU32(color), msText);
    // 単位と fps は大きい数字の下端へ揃える (ベースラインを合わせると小さい方が浮く)。
    const float subY = origin.y + heroSize - fontH;
    dl->AddText(font, fontH, { origin.x + heroDim.x + fontH * 0.2f, subY },
                EditorTheme::ColorU32(ThemeColor::TextMuted), "ms");
    dl->AddText(font, fontH, { origin.x + rowW - ImGui::CalcTextSize(fpsText).x, subY },
                EditorTheme::ColorU32(ThemeColor::TextMuted), fpsText);
    return color;
}

void FrameTimeGraph(float targetMs, float height)
{
    SampleFrameTime();

    const float  width  = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({ width, height });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, { origin.x + width, origin.y + height },
                      EditorTheme::ColorU32(ThemeColor::Field, 0.55f), 3.0f);

    // 予算の 1.5 倍を最低スケールにして、軽いときに予算線が潰れないようにする。
    float scaleMs = targetMs * 1.5f;
    for (int i = 0; i < kHistory; ++i)
        scaleMs = std::max(scaleMs, s_history[i]);

    const float plotH = height - 2.0f;
    const float barW  = width / static_cast<float>(kHistory);
    for (int i = 0; i < kHistory; ++i) {
        const float ms = s_history[(s_head + i) % kHistory];
        if (ms <= 0.0f) continue;
        const float h = std::min(ms / scaleMs, 1.0f) * plotH;
        const float x = origin.x + barW * static_cast<float>(i);
        // 隙間を空けず面グラフとして描く。HUD 側は 1 サンプル 2px 前後しかなく、
        // 棒に割ると 1px を切って形が読めなくなる。
        dl->AddRectFilled({ x, origin.y + height - h }, { x + barW + 0.5f, origin.y + height },
                          ms > targetMs ? EditorTheme::ColorU32(ThemeColor::Danger, 0.85f)
                                        : EditorTheme::ColorU32(ThemeColor::Accent, 0.70f));
    }

    const float budgetY = origin.y + height - (targetMs / scaleMs) * plotH;
    dl->AddLine({ origin.x, budgetY }, { origin.x + width, budgetY },
                EditorTheme::ColorU32(ThemeColor::TextFaint, 0.9f), 1.0f);
}

} // namespace fbzz::editor::widgets
