/// @file    RenderDecalInput.hpp
/// @brief   デカールの解決済み投影・材質入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
namespace fbzz::renderer {
struct RenderDecalInput {
    uint32_t layer = 0;
    int sortOrder = 0;
    DecalCB projection{};
    DecalMaterialCB material{};
    DrawCall binding{};
    bool customMaterial = false;
    std::vector<uint8_t> materialParameters;
};
}
