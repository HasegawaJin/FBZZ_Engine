/// @file    MaterialPreviewChoices.hpp
/// @brief   材質プレビューの操作登録簿と画面で共有する選択肢。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#pragma once
#include <Editor/Panels/MaterialPreview.hpp>
#include <array>

namespace fbzz::editor::previewchoices {

inline constexpr std::array<matpreview::Shape, 7> SHAPES = {
    matpreview::Shape::Sphere, matpreview::Shape::Cube, matpreview::Shape::Plane, matpreview::Shape::Cylinder,
    matpreview::Shape::Cone,   matpreview::Shape::Torus, matpreview::Shape::Capsule,
};

inline constexpr std::array<matpreview::Channel, 10> CHANNELS = {
    matpreview::Channel::Shaded,    matpreview::Channel::Albedo,    matpreview::Channel::Normal,
    matpreview::Channel::Roughness, matpreview::Channel::Metallic,  matpreview::Channel::Occlusion,
    matpreview::Channel::Emissive,  matpreview::Channel::Uv,        matpreview::Channel::HeightMap, matpreview::Channel::Wireframe,
};

inline constexpr std::array<matpreview::FiberMode, 4> FIBER_MODES = {
    matpreview::FiberMode::Shell, matpreview::FiberMode::Fin, matpreview::FiberMode::Hybrid, matpreview::FiberMode::Blade,
};

inline constexpr std::array<matpreview::LightPreset, 4> LIGHT_PRESETS = {
    matpreview::LightPreset::Studio, matpreview::LightPreset::Outdoor,
    matpreview::LightPreset::Night,  matpreview::LightPreset::Flat,
};

inline constexpr std::array<MaterialPreviewBackground, 4> BACKGROUNDS = {
    MaterialPreviewBackground::Gradient, MaterialPreviewBackground::Checker,
    MaterialPreviewBackground::Solid,    MaterialPreviewBackground::Grid,
};

inline const char* BackgroundLabel(MaterialPreviewBackground background)
{
    switch (background) {
        case MaterialPreviewBackground::Checker: return "Checker";
        case MaterialPreviewBackground::Solid:   return "Solid";
        case MaterialPreviewBackground::Grid:    return "Grid";
        case MaterialPreviewBackground::Gradient:
        default:                                 return "Gradient";
    }
}

}
