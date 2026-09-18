/// @file    VFXLineInspector.cpp
/// @brief   VFX Line の Inspector 部品 (プリセット・形のプレビュー)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Editor/Util/VFXLineInspector.hpp>

#include <Math/CurlNoise.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/VFXLineGeometry.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::editor {
namespace {

constexpr float kPreviewHeight = 110.0f;
constexpr float kPreviewMargin = 14.0f;

/// HDR の色を表示色へ。明るさが 1 を超えた分は白へ寄せる (ブルームの見え方に近い)。
ImU32 DisplayColor(const math::Vector4& color, float scale, float alpha)
{
    const auto tone = [scale](float c) { return 1.0f - std::exp(-(std::max)(c, 0.0f) * scale); };
    return ImGui::ColorConvertFloat4ToU32({ tone(color.x), tone(color.y), tone(color.z), std::clamp(alpha, 0.0f, 1.0f) });
}

} // namespace

bool DrawVFXLinePresetBar(scene::VFXLineComponent& line)
{
    bool changed = false;
    ImGui::TextDisabled("Preset");
    for (int i = 0; i < static_cast<int>(scene::VFXLinePreset::Count); ++i) {
        const auto preset = static_cast<scene::VFXLinePreset>(i);
        ImGui::SameLine();
        if (ImGui::SmallButton(scene::VFXLinePresetName(preset))) {
            scene::ApplyVFXLinePreset(line, preset);
            changed = true;
        }
    }
    return changed;
}

void DrawVFXLinePreview(const scene::VFXLineComponent& line)
{
    const float width = (std::max)(ImGui::GetContentRegionAvail().x, 120.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##vfx_line_preview", { width, kPreviewHeight });
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, { origin.x + width, origin.y + kPreviewHeight }, IM_COL32(16, 18, 24, 255), 4.0f);

    /// @note 端点はプレビュー用に横一文字へ固定する。形の規則 (折れ・枝・打ち直し・明滅) は本番と同じ関数。
    const float time = static_cast<float>(ImGui::GetTime());
    scene::VFXLineComponent preview = line;
    std::vector<scene::VFXLineStrand> strands;
    const math::Vector3 from{ -1.0f, 0.0f, 0.0f };
    const math::Vector3 to{ 1.0f, 0.0f, 0.0f };
    std::uint32_t strikeIndex = 0;
    float strikeClock = time;
    if (line.mode == scene::VFXLineMode::Lightning) {
        if (line.strikeRate > 0.0f) {
            strikeIndex = static_cast<std::uint32_t>(time * line.strikeRate);
            strikeClock = time - static_cast<float>(strikeIndex) / line.strikeRate;
        }
        const std::uint32_t seed = math::PcgHash(static_cast<std::uint32_t>(line.seed) * 9781u + strikeIndex * 6271u);
        scene::GenerateLightning(preview, from, to, seed, time, strands);
    } else {
        /// @note 実寸のたるみ・揺れを «長さ 2 の線» へ縮めて見せる。
        const float length = (std::max)(line.toPoint.Length(), 0.5f);
        preview.sag = line.sag * 2.0f / length;
        preview.wobble = line.wobble * 2.0f / length;
        scene::GenerateBeam(preview, from, to, time, strands);
    }
    const float brightness = scene::VFXLineBrightness(line, time, strikeIndex, strikeClock);

    const float usable = width - kPreviewMargin * 2.0f;
    const float pixelsPerUnit = usable * 0.5f;
    const ImVec2 center = { origin.x + width * 0.5f, origin.y + kPreviewHeight * 0.5f };
    /// @note 幅は «線の長さに対する割合» で見せる (実際の長さは端点しだいなので toPoint の長さで代用する)。
    const float relativeWidth = line.width / (std::max)(line.toPoint.Length(), 0.5f) * 2.0f;
    std::vector<ImVec2> points;
    drawList->PushClipRect(origin, { origin.x + width, origin.y + kPreviewHeight }, true);
    for (const scene::VFXLineStrand& strand : strands) {
        points.clear();
        for (const math::Vector3& p : strand.points)
            points.push_back({ center.x + p.x * pixelsPerUnit, center.y - p.y * pixelsPerUnit });
        if (points.size() < 2) continue;
        const float thickness = std::clamp(relativeWidth * strand.width * pixelsPerUnit, 1.0f, 28.0f);
        const float glow = line.intensity * brightness * strand.brightness;
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), DisplayColor(line.color, glow * 0.25f, 0.45f),
                              ImDrawFlags_None, thickness * 2.2f);
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), DisplayColor(line.color, glow * 0.6f, 0.9f),
                              ImDrawFlags_None, thickness);
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()),
                              DisplayColor(line.coreColor, glow * 0.8f, 1.0f), ImDrawFlags_None,
                              (std::max)(thickness * std::clamp(line.coreWidth, 0.05f, 1.0f), 1.0f));
    }
    drawList->PopClipRect();
    drawList->AddText({ origin.x + 6.0f, origin.y + 4.0f }, IM_COL32(200, 200, 210, 200),
                      line.mode == scene::VFXLineMode::Lightning ? "Lightning" : "Beam");
}

} // namespace fbzz::editor
