/// @file    ContentGlyph.cpp
/// @brief   小さい表示でも輪郭で判別できるエディター用の図形記号。
/// @author  Hasegawa Jin
/// @date    2026-11-24
#include "ContentGlyph.hpp"
#include <Engine/Util/Text/StringUtils.hpp>
#include <algorithm>

namespace fbzz::editor::ui
{

ContentGlyph FolderGlyph(std::string_view name)
{
    struct Entry
    {
        std::string_view name;
        ContentGlyph glyph;
    };
    static constexpr Entry folders[] = {
        {"Materials", ContentGlyph::MATERIAL}, {"Scenes", ContentGlyph::SCENE},
        {"Scripts", ContentGlyph::SCRIPT},     {"Shaders", ContentGlyph::SHADER},
        {"Textures", ContentGlyph::IMAGE},     {"Sprites", ContentGlyph::IMAGE},
        {"Prefabs", ContentGlyph::PREFAB},     {"Meshes", ContentGlyph::MESH},
        {"Models", ContentGlyph::MESH},        {"Animations", ContentGlyph::ANIMATION},
        {"Audio", ContentGlyph::AUDIO},        {"Terrain", ContentGlyph::TERRAIN},
        {"Effects", ContentGlyph::EFFECTS},    {"VFX", ContentGlyph::EFFECTS},
        {"Particles", ContentGlyph::EFFECTS},  {"UI", ContentGlyph::CANVAS},
        {"Fonts", ContentGlyph::FONT},         {"Data", ContentGlyph::DATA},
        {"Physics", ContentGlyph::PHYSICS},
    };
    for (const auto &entry : folders)
        if (util::StringUtils::EqualsCIView(name, entry.name))
            return entry.glyph;
    return ContentGlyph::NONE;
}

void DrawContentGlyph(ContentGlyph type, ImVec2 center, float size, ImU32 color)
{
    if (type == ContentGlyph::NONE || size <= 0.0f)
        return;
    auto &draw = *ImGui::GetWindowDrawList();
    const float scale = size / 16.0f;
    const float stroke = std::max(1.0f, 1.45f * scale);
    const auto point = [&](float x, float y) {
        return ImVec2(center.x + (x - 8) * scale, center.y + (y - 8) * scale);
    };
    const auto line = [&](float x, float y, float u, float v) {
        draw.AddLine(point(x, y), point(u, v), color, stroke);
    };
    const auto rect = [&](float x, float y, float u, float v) {
        draw.AddRect(point(x, y), point(u, v), color, 1.2f * scale, stroke);
    };
    const auto circle = [&](float x, float y, float r) {
        draw.AddCircle(point(x, y), r * scale, color, 16, stroke);
    };
    const auto solid = [&](float x, float y, float r) { draw.AddCircleFilled(point(x, y), r * scale, color, 12); };
    switch (type)
    {
    case ContentGlyph::OBJECT:
        line(8, 2, 14, 8);
        line(14, 8, 8, 14);
        line(8, 14, 2, 8);
        line(2, 8, 8, 2);
        break;
    case ContentGlyph::MESH: {
        const ImVec2 outline[] = {point(8, 1.5f), point(14, 5),    point(14, 11.5f),
                                  point(8, 15),   point(2, 11.5f), point(2, 5)};
        draw.AddPolyline(outline, 6, color, stroke, ImDrawFlags_Closed);
        line(2, 5, 8, 8.5f);
        line(8, 8.5f, 14, 5);
        line(8, 8.5f, 8, 15);
        break;
    }
    case ContentGlyph::PREFAB:
        rect(6, 1.5f, 10, 5.5f);
        line(8, 5.5f, 8, 8);
        line(3.5f, 8, 12.5f, 8);
        line(3.5f, 8, 3.5f, 10);
        line(12.5f, 8, 12.5f, 10);
        rect(1, 10, 6, 14.5f);
        rect(10, 10, 15, 14.5f);
        break;
    case ContentGlyph::FOLDER: {
        const ImVec2 outline[] = {point(1, 3.5f), point(6, 3.5f), point(7.5f, 5.5f),
                                  point(15, 5.5f), point(15, 13), point(1, 13)};
        draw.AddPolyline(outline, 6, color, stroke, ImDrawFlags_Closed);
        break;
    }
    case ContentGlyph::EFFECTS: {
        const ImVec2 flare[] = {point(6, 2), point(7.5f, 6.5f), point(12, 8), point(7.5f, 9.5f),
                                point(6, 14), point(4.5f, 9.5f), point(0, 8), point(4.5f, 6.5f)};
        draw.AddPolyline(flare, 8, color, stroke, ImDrawFlags_Closed);
        line(12.5f, 1, 12.5f, 5);
        line(10.5f, 3, 14.5f, 3);
        break;
    }
    case ContentGlyph::CANVAS:
        rect(1, 2, 15, 14);
        line(1, 5.5f, 15, 5.5f);
        line(6, 5.5f, 6, 14);
        break;
    case ContentGlyph::FONT:
        line(2, 5, 2, 3);
        line(2, 3, 14, 3);
        line(14, 3, 14, 5);
        line(8, 3, 8, 14);
        line(5, 14, 11, 14);
        break;
    case ContentGlyph::DATA:
        draw.AddEllipse(point(8, 4), {6 * scale, 2.5f * scale}, color, 0.0f, 16, stroke);
        line(2, 4, 2, 11.5f);
        line(14, 4, 14, 11.5f);
        draw.AddBezierCubic(point(2, 11.5f), point(3, 15), point(13, 15), point(14, 11.5f), color, stroke);
        break;
    case ContentGlyph::PHYSICS:
        circle(9, 6, 4);
        line(1, 14, 15, 11);
        break;
    case ContentGlyph::CAMERA:
        rect(1, 4, 10, 12);
        line(10, 7, 15, 4);
        line(15, 4, 15, 12);
        line(15, 12, 10, 9);
        break;
    case ContentGlyph::LIGHT:
        circle(8, 8, 3);
        line(8, 1, 8, 3);
        line(8, 13, 8, 15);
        line(1, 8, 3, 8);
        line(13, 8, 15, 8);
        line(3, 3, 4.5f, 4.5f);
        line(11.5f, 11.5f, 13, 13);
        line(3, 13, 4.5f, 11.5f);
        line(11.5f, 4.5f, 13, 3);
        break;
    case ContentGlyph::TERRAIN:
        line(1, 13, 6, 4);
        line(6, 4, 9, 9);
        line(9, 9, 11, 6);
        line(11, 6, 15, 13);
        line(15, 13, 1, 13);
        break;
    case ContentGlyph::WATER:
        for (float y : {5.f, 10.f})
        {
            draw.AddBezierCubic(point(1, y + 1), point(4, y - 4), point(5, y - 2), point(8, y), color, stroke, 8);
            draw.AddBezierCubic(point(8, y), point(11, y + 2), point(12, y + 3), point(15, y - 1), color, stroke,
                                8);
        }
        break;
    case ContentGlyph::MATERIAL:
        circle(8, 8, 6);
        draw.AddBezierCubic(point(3, 11), point(9, 11), point(12, 7), point(12, 4), color, stroke, 12);
        solid(6, 6, 1);
        break;
    case ContentGlyph::SCRIPT:
        line(5, 4, 1, 8);
        line(1, 8, 5, 12);
        line(11, 4, 15, 8);
        line(15, 8, 11, 12);
        line(9.5f, 2, 6.5f, 14);
        break;
    case ContentGlyph::SHADER:
        line(9, 1, 2, 9);
        line(2, 9, 7.5f, 9);
        line(7.5f, 9, 6, 15);
        line(6, 15, 14, 6);
        line(14, 6, 8.5f, 6);
        line(8.5f, 6, 9, 1);
        break;
    case ContentGlyph::IMAGE:
        rect(1.5f, 2.5f, 14.5f, 13.5f);
        solid(5, 6, 1.3f);
        line(3, 12, 7, 8);
        line(7, 8, 9, 10);
        line(9, 10, 12, 7);
        line(12, 7, 14, 9);
        break;
    case ContentGlyph::SCENE: {
        const ImVec2 slate[] = {point(1.5f, 3.5f), point(13.5f, 1), point(14, 4), point(2, 6.5f)};
        draw.AddPolyline(slate, 4, color, stroke, ImDrawFlags_Closed);
        line(6, 3, 5, 5);
        line(10.5f, 2, 9.5f, 4);
        rect(2, 7, 14, 14);
        line(5, 10.5f, 10, 10.5f);
    }
        break;
    case ContentGlyph::AUDIO:
        line(6, 11, 6, 4);
        line(6, 4, 13, 2.5f);
        line(13, 2.5f, 13, 10);
        solid(4, 12, 2.4f);
        solid(11, 11, 2.4f);
        break;
    case ContentGlyph::ANIMATION:
        rect(1.5f, 3, 14.5f, 13);
        draw.AddTriangleFilled(point(6.5f, 5), point(6.5f, 11), point(11, 8), color);
        break;
    case ContentGlyph::EYE:
    case ContentGlyph::EYE_OFF:
        draw.AddBezierCubic(point(1, 8), point(4, 1), point(12, 1), point(15, 8), color, stroke, 12);
        draw.AddBezierCubic(point(1, 8), point(4, 15), point(12, 15), point(15, 8), color, stroke, 12);
        if (type == ContentGlyph::EYE_OFF)
            line(2, 2, 14, 14);
        else
            solid(8, 8, 2.1f);
        break;
    case ContentGlyph::ENABLED:
    case ContentGlyph::DISABLED:
        draw.AddBezierCubic(point(5, 3.8f), point(-1, 7), point(2, 14), point(8, 14), color, stroke, 12);
        draw.AddBezierCubic(point(8, 14), point(14, 14), point(17, 7), point(11, 3.8f), color, stroke, 12);
        if (type == ContentGlyph::ENABLED)
            line(8, 1, 8, 8);
        else
            line(2, 2, 14, 14);
        break;
    case ContentGlyph::LOCK:
    case ContentGlyph::UNLOCK:
        rect(3, 7, 13, 14);
        draw.AddBezierCubic(point(5, 7), point(4, 0), point(12, 0),
                            point(11, type == ContentGlyph::LOCK ? 7.f : 4.f), color, stroke, 12);
        solid(8, 10.5f, 1);
        break;
    case ContentGlyph::NONE:
        break;
    }
}

}
