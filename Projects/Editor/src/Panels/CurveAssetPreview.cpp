/// @file    CurveAssetPreview.cpp
/// @brief   .curve / .gradient のグラフと色帯を描画する。
/// @author  Hasegawa Jin
/// @date    2026-09-26
#include <Editor/Panels/CurveAssetPreview.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fbzz::editor::curvepreview {
namespace {

float FiniteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

ImU32 ToImColor(const math::Vector4& color)
{
    return ImGui::ColorConvertFloat4ToU32({
        std::clamp(FiniteOr(color.x, 0.0f), 0.0f, 1.0f),
        std::clamp(FiniteOr(color.y, 0.0f), 0.0f, 1.0f),
        std::clamp(FiniteOr(color.z, 0.0f), 0.0f, 1.0f),
        std::clamp(FiniteOr(color.w, 0.0f), 0.0f, 1.0f) });
}

} /// @note anonymous namespace

void DrawCurve(ImDrawList* draw, const scene::ParticleCurve& curve, ImVec2 origin, ImVec2 size)
{
    if (!draw || size.x <= 0.0f || size.y <= 0.0f) return;
    const ImVec2 max{ origin.x + size.x, origin.y + size.y };
    draw->PushClipRect(origin, max, true);
    draw->AddRectFilled(origin, max, IM_COL32(22, 25, 32, 255), 3.0f);

    float minimum = 0.0f;
    float maximum = 0.0f;
    const uint32_t count = std::min<uint32_t>(curve.keyCount, static_cast<uint32_t>(curve.keys.size()));
    for (uint32_t i = 0; i < count; ++i) {
        const float value = curve.keys[i].value;
        if (!std::isfinite(value)) continue;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    /// @note 定数曲線でも上下の余白を確保し、ゼロ線と曲線を判別できるようにする。
    const float padding = std::max((maximum - minimum) * 0.12f, 0.1f);
    minimum -= padding;
    maximum += padding;
    const float range = maximum - minimum;
    auto toY = [&](float value) {
        return max.y - (FiniteOr(value, 0.0f) - minimum) / range * size.y;
    };

    for (int i = 1; i < 4; ++i) {
        const float x = origin.x + size.x * static_cast<float>(i) * 0.25f;
        draw->AddLine({ x, origin.y }, { x, max.y }, IM_COL32(255, 255, 255, 18));
    }
    if (minimum <= 0.0f && maximum >= 0.0f)
        draw->AddLine({ origin.x, toY(0.0f) }, { max.x, toY(0.0f) },
                      IM_COL32(255, 255, 255, 55));

    const int samples = std::clamp(static_cast<int>(size.x), 24, 256);
    ImVec2 previous{ origin.x, toY(curve.Evaluate(0.0f)) };
    for (int i = 1; i <= samples; ++i) {
        const float time = static_cast<float>(i) / static_cast<float>(samples);
        const ImVec2 current{ origin.x + size.x * time, toY(curve.Evaluate(time)) };
        draw->AddLine(previous, current, IM_COL32(120, 200, 255, 255), 2.0f);
        previous = current;
    }

    if (size.x >= 70.0f && size.y >= 60.0f) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!std::isfinite(curve.keys[i].time) || !std::isfinite(curve.keys[i].value)) continue;
            const ImVec2 point{ origin.x + std::clamp(curve.keys[i].time, 0.0f, 1.0f) * size.x,
                                toY(curve.keys[i].value) };
            draw->AddCircleFilled(point, 3.0f, IM_COL32(240, 245, 255, 255));
        }
    }
    draw->AddRect(origin, max, IM_COL32(90, 96, 110, 255), 3.0f);
    draw->PopClipRect();
}

void DrawGradient(ImDrawList* draw, const scene::ParticleGradient& gradient,
                  ImVec2 origin, ImVec2 size)
{
    if (!draw || size.x <= 0.0f || size.y <= 0.0f) return;
    const ImVec2 max{ origin.x + size.x, origin.y + size.y };
    draw->PushClipRect(origin, max, true);
    constexpr float checkerSize = 8.0f;
    for (int x = 0; x < static_cast<int>(size.x / checkerSize) + 1; ++x) {
        for (int y = 0; y < static_cast<int>(size.y / checkerSize) + 1; ++y) {
            const ImVec2 cellMin{ origin.x + x * checkerSize, origin.y + y * checkerSize };
            const ImVec2 cellMax{ std::min(cellMin.x + checkerSize, max.x),
                                  std::min(cellMin.y + checkerSize, max.y) };
            draw->AddRectFilled(cellMin, cellMax,
                                (x + y) % 2 == 0 ? IM_COL32(64, 64, 64, 255)
                                                 : IM_COL32(104, 104, 104, 255));
        }
    }
    /// @note Evaluate は色空間と Step / Smooth 補間を反映する。透明度は市松背景へ重ねる。
    const int samples = std::clamp(static_cast<int>(size.x), 24, 256);
    for (int i = 0; i < samples; ++i) {
        const float time = (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
        const float x0 = origin.x + size.x * static_cast<float>(i) / static_cast<float>(samples);
        const float x1 = origin.x + size.x * static_cast<float>(i + 1) / static_cast<float>(samples);
        draw->AddRectFilled({ x0, origin.y }, { x1, max.y }, ToImColor(gradient.Evaluate(time)));
    }
    draw->AddRect(origin, max, IM_COL32(90, 96, 110, 255), 3.0f);
    draw->PopClipRect();
}

} /// @note namespace fbzz::editor::curvepreview
