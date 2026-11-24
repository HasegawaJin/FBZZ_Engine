/// @file    MaterialPreviewToolbar.cpp
/// @brief   材質プレビューの形状選択・表示チャンネル・撮影設定。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#include "MaterialPreviewChoices.hpp"
#include <Editor/UI/Localization.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace fbzz::editor {
namespace {

namespace mp = matpreview;
namespace choices = previewchoices;

const char* FixedGeometry(mp::Flavor flavor)
{
    switch (flavor) {
        case mp::Flavor::Terrain:
        case mp::Flavor::Water:       return "Plane";
        case mp::Flavor::Ui:          return "Rect";
        case mp::Flavor::Particle:    return "Billboard";
        case mp::Flavor::Trail:       return "Ribbon";
        case mp::Flavor::Decal:       return "Projected";
        case mp::Flavor::PostProcess: return "Fullscreen";
        case mp::Flavor::Unsupported: return "Swatch";
        default:                      return nullptr;
    }
}

/// @note 輪郭と面の陰影を同じ縮尺で描き、フォントや GPU テクスチャの有無に依存しない。
void DrawShapeGlyph(mp::Shape shape, ImVec2 center, float radius)
{
    auto& draw = *ImGui::GetWindowDrawList();
    const auto p = [&](float x, float y) { return ImVec2(center.x + x * radius, center.y + y * radius); };
    const auto ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const auto tone = [&](float shade) {
        return ImGui::GetColorU32({ ink.x * shade, ink.y * shade, ink.z * shade, ink.w });
    };
    const auto light = tone(1.0f), mid = tone(0.76f), dark = tone(0.50f);
    const float line = std::max(1.0f, radius * 0.12f);
    switch (shape) {
        case mp::Shape::Sphere:
            draw.AddCircleFilled(center, radius * 0.96f, mid, 24);
            draw.AddBezierCubic(p(-0.77f, 0.41f), p(-0.94f, -0.07f), p(-0.50f, -0.76f), p(0.10f, -0.80f), light, line);
            draw.AddBezierCubic(p(0.76f, -0.36f), p(0.89f, 0.15f), p(0.48f, 0.75f), p(-0.05f, 0.79f), dark, line);
            break;
        case mp::Shape::Cube:
            draw.AddQuadFilled(p(0, -1), p(0.91f, -0.52f), p(0, -0.03f), p(-0.91f, -0.52f), light);
            draw.AddQuadFilled(p(-0.91f, -0.52f), p(0, -0.03f), p(0, 1), p(-0.91f, 0.51f), mid);
            draw.AddQuadFilled(p(0, -0.03f), p(0.91f, -0.52f), p(0.91f, 0.51f), p(0, 1), dark);
            break;
        case mp::Shape::Plane:
        case mp::Shape::Quad:
            draw.AddQuadFilled(p(0.22f, -0.66f), p(1.0f, -0.10f), p(-0.22f, 0.66f), p(-1.0f, 0.10f), mid);
            draw.AddLine(p(-0.39f, -0.28f), p(0.39f, 0.28f), light, line);
            draw.AddLine(p(-0.61f, 0.38f), p(0.61f, -0.38f), light, line);
            break;
        case mp::Shape::Cylinder: {
            const ImVec2 cap{ 0.77f * radius, 0.32f * radius };
            draw.AddEllipseFilled(p(0, 0.64f), cap, mid);
            draw.AddRectFilled(p(-0.77f, -0.64f), p(0.77f, 0.64f), mid);
            draw.AddEllipseFilled(p(0, -0.64f), cap, light);
            break;
        }
        case mp::Shape::Cone:
            draw.AddEllipseFilled(p(0, 0.61f), { radius * 0.92f, radius * 0.31f }, mid);
            draw.AddTriangleFilled(p(0, -1), p(0.92f, 0.61f), p(-0.92f, 0.61f), mid);
            draw.AddTriangleFilled(p(0, -1), p(-0.16f, 0.72f), p(-0.92f, 0.61f), light);
            break;
        case mp::Shape::Torus: {
            const ImVec2 ring{ radius * 0.74f, radius * 0.45f };
            draw.AddEllipse(p(0, 0.15f), ring, dark, -0.32f, 32, radius * 0.47f);
            draw.AddEllipse(p(0, -0.09f), ring, light, -0.32f, 32, radius * 0.43f);
            break;
        }
        case mp::Shape::Capsule:
            draw.AddRectFilled(p(-0.55f, -1), p(0.55f, 1), mid, radius * 0.55f);
            draw.AddBezierCubic(p(-0.35f, 0.28f), p(-0.35f, -0.42f), p(-0.41f, -0.69f), p(-0.04f, -0.80f), light, line);
            break;
    }
}

void DrawShapeStrip(mp::Shape& current)
{
    const auto& style = ImGui::GetStyle();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float font = ImGui::GetFontSize();
    const float gap = style.ItemInnerSpacing.x;
    const int columns = std::clamp(static_cast<int>((width + gap) / (font * 2.45f + gap)),
                                   1, static_cast<int>(choices::SHAPES.size()));
    const float tileWidth = std::min(font * 6.0f, std::floor((width - (columns - 1) * gap) / columns));
    float labelWidth = 0.0f;
    for (const auto shape : choices::SHAPES)
        labelWidth = std::max(labelWidth, ImGui::CalcTextSize(LOCT(mp::ShapeLabel(shape))).x);
    const bool captions = tileWidth >= labelWidth + gap;
    const float glyphHeight = font * 1.55f;
    const float tileHeight = glyphHeight + style.FramePadding.y * 2.0f
                           + (captions ? font + style.ItemInnerSpacing.y : 0.0f);
    auto& draw = *ImGui::GetWindowDrawList();
    for (size_t index = 0; index < choices::SHAPES.size(); ++index) {
        if (index % columns != 0) ImGui::SameLine(0.0f, gap);
        const auto shape = choices::SHAPES[index];
        const char* name = mp::ShapeLabel(shape);
        if (ImGui::InvisibleButton(name, { tileWidth, tileHeight }, ImGuiButtonFlags_EnableNav)) current = shape;
        const bool selected = current == shape;
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        const auto fill = ImGui::IsItemActive() ? ImGuiCol_ButtonActive :
                          ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered :
                          selected ? ImGuiCol_Header : ImGuiCol_FrameBg;
        draw.AddRectFilled(min, max, ImGui::GetColorU32(fill), style.FrameRounding);
        if (selected)
            draw.AddRect(min, max, ImGui::GetColorU32(ImGuiCol_CheckMark), style.FrameRounding);
        const ImVec2 center{ (min.x + max.x) * 0.5f, min.y + style.FramePadding.y + glyphHeight * 0.5f };
        DrawShapeGlyph(shape, center, std::min(glyphHeight, tileWidth) * 0.45f);
        const char* caption = LOCT(name);
        if (captions) {
            const float x = std::floor(center.x - ImGui::CalcTextSize(caption).x * 0.5f);
            draw.AddText({ x, max.y - style.FramePadding.y - font }, ImGui::GetColorU32(ImGuiCol_Text), caption);
        }
        ImGui::SetItemTooltip("%s", caption);
    }
}

template<typename T, size_t N>
void DrawChoice(const char* id, T& current, const std::array<T, N>& options, const char* (*label)(T))
{
    if (!ImGui::BeginCombo(id, LOCT(label(current)))) return;
    for (const auto value : options) {
        const bool selected = current == value;
        if (ImGui::Selectable(LOC(label(value)), selected)) current = value;
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

void DrawFieldLabel(const char* label)
{
    ImGui::TextDisabled("%s", LOCT(label));
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void DrawParallaxToggle(bool& enabled, bool hasHeightMap, bool active)
{
    ImGui::BeginDisabled(!active);
    bool show = active && enabled;
    if (ImGui::Checkbox(LOC("Parallax (POM)"), &show)) enabled = show;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", LOCT(!hasHeightMap ? "Assign a Height Map in the Material Inspector." :
            !active ? "Set Parallax Depth above 0 cm in the Material Inspector." :
                      "Compare parallax at the current view angle."));
}

} /// @note namespace

void MaterialPreviewView::DrawToolbar(mp::Flavor flavor, bool gpuReady)
{
    if (const char* fixed = FixedGeometry(flavor)) {
        ImGui::TextDisabled("%s", LOCT("Geometry"));
        ImGui::SameLine();
        ImGui::TextUnformatted(LOCT(fixed));
        ImGui::SetItemTooltip("%s", LOCT("This material uses fixed preview geometry."));
    } else {
        DrawShapeStrip(m_shape);
    }

    const bool supportsParallax = gpuReady && m_gpu.parallaxDepthOffset != UINT32_MAX
        && (flavor == mp::Flavor::Surface || flavor == mp::Flavor::Skinned);
    const bool hasHeightMap = supportsParallax && m_gpu.textures.size() > 5 && m_gpu.textures[5].IsValid();
    float depth = 0.0f;
    if (hasHeightMap)
        std::memcpy(&depth, m_gpu.paramData.data() + m_gpu.parallaxDepthOffset, sizeof(depth));
    const bool parallaxActive = depth > 0.0f;
    if (!mp::ChannelSupported(flavor, m_channel) || (m_channel == mp::Channel::HeightMap && !hasHeightMap))
        m_channel = mp::Channel::Shaded;

    const auto& style = ImGui::GetStyle();
    const float comboPadding = ImGui::GetFrameHeight() + 2.0f * style.FramePadding.x;
    const float settingsWidth = ImGui::CalcTextSize(LOCT("Settings")).x + comboPadding;
    const bool hasChannels = std::any_of(choices::CHANNELS.begin(), choices::CHANNELS.end(), [flavor](mp::Channel channel) {
        return channel != mp::Channel::Shaded && mp::ChannelSupported(flavor, channel);
    });
    if (hasChannels) {
        const char* channelCaption = LOCT(mp::ChannelLabel(m_channel));
        const float minCombo = std::max(ImGui::CalcTextSize(LOCT("Height Map")).x, ImGui::CalcTextSize(channelCaption).x) + comboPadding;
        const float controlsWidth = minCombo + settingsWidth + style.ItemSpacing.x;
        const float width = ImGui::GetContentRegionAvail().x;
        if (width >= controlsWidth + ImGui::CalcTextSize(LOCT("Channel")).x + style.ItemSpacing.x) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", LOCT("Channel"));
            ImGui::SameLine();
        }
        const bool sameRow = width >= controlsWidth;
        ImGui::SetNextItemWidth(sameRow ? ImGui::GetContentRegionAvail().x - settingsWidth - style.ItemSpacing.x : -FLT_MIN);
        if (ImGui::BeginCombo("##Channel", channelCaption)) {
            for (auto channel : choices::CHANNELS) {
                if (channel == mp::Channel::HeightMap && !hasHeightMap) continue;
                const bool selected = channel == m_channel;
                ImGui::BeginDisabled(!mp::ChannelSupported(flavor, channel));
                if (ImGui::Selectable(LOC(mp::ChannelLabel(channel)), selected)) m_channel = channel;
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", LOCT("Channel"));
        if (sameRow) ImGui::SameLine();
    }

    const auto workSize = ImGui::GetWindowViewport()->WorkSize;
    const float popupWidth = std::min(ImGui::GetFontSize() * 17.0f, workSize.x);
    ImGui::SetNextWindowSizeConstraints({ popupWidth, 0.0f }, { popupWidth, workSize.y });
    ImGui::SetNextItemWidth(std::min(settingsWidth, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##PreviewSettings", LOCT("Settings"))) {
        if (flavor != mp::Flavor::Unsupported) {
            DrawFieldLabel("Lighting");
            DrawChoice("##Light", m_rig.preset, choices::LIGHT_PRESETS, mp::LightPresetLabel);
            DrawFieldLabel("Exposure multiplier");
            ImGui::SliderFloat("##Exposure", &m_rig.exposure, 0.1f, 4.0f, "%.2f x", ImGuiSliderFlags_AlwaysClamp);
        }
        DrawFieldLabel("Background");
        DrawChoice("##Background", m_background, choices::BACKGROUNDS, choices::BackgroundLabel);
        if (m_background == MaterialPreviewBackground::Solid) {
            DrawFieldLabel("Background color");
            ImGui::ColorEdit3("##BackgroundColor", m_solidColor, ImGuiColorEditFlags_NoLabel);
        }
        if (flavor == mp::Flavor::Fiber) {
            ImGui::Separator();
            DrawFieldLabel("Fiber mode");
            DrawChoice("##FiberMode", m_fiberMode, choices::FIBER_MODES, mp::FiberModeLabel);
            ImGui::BeginDisabled(m_fiberMode == mp::FiberMode::Fin || m_fiberMode == mp::FiberMode::Blade);
            DrawFieldLabel("Shells");
            ImGui::SliderInt("##FiberShells", &m_fiberShellCount, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            DrawFieldLabel("Wind speed");
            ImGui::SliderFloat("##FiberWind", &m_fiberWind, 0.0f, 10.0f, "%.1f m/s", ImGuiSliderFlags_AlwaysClamp);
        }
        if (flavor != mp::Flavor::Unsupported) {
            ImGui::Separator();
            if (supportsParallax && !parallaxActive) DrawParallaxToggle(m_showParallax, hasHeightMap, false);
            if (ImGui::Button(LOC("Reset view"), { -FLT_MIN, 0 })) ResetView();
            ImGui::SetItemTooltip("%s", LOCT("Restore the camera, lighting, channel and parallax preview."));
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", LOCT("Preview settings"));
    if (parallaxActive) DrawParallaxToggle(m_showParallax, hasHeightMap, true);
    if (mp::UsesPreviewTime(flavor, true)) ImGui::Checkbox(LOC("Animate"), &m_animateWater);
}

} /// @note namespace fbzz::editor
