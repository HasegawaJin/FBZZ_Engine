/// @file    RenderMaterial.hpp
/// @brief   描画とレイで読み取る解決済み材質。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <Graphics/RayTracing/RayMaterialCapabilities.hpp>
#include <Graphics/RayTracing/SurfaceMaterialData.hpp>
#include <Graphics/Renderer/GeometryRoute.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/Renderer/ShaderCapabilities.hpp>
#include <array>
#include <cstdint>

namespace fbzz::renderer {

/// @brief 抽出時に解決した、このビューの材質入力。
/// @note GPU ハンドルは非所有。材質の有効性は資源解放後の参照を保証しない。
struct RenderMaterial {
    ShaderCapabilities shaderCapabilities;
    GeometryMaterialInput capabilities;
    RayMaterialCapabilities rayCapabilities;
    SurfaceMaterialData surface;
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<ConstantBufferTag> paramsBuffer;
    std::array<ResourceHandle<TextureTag>, 8> textures{};
    std::array<uint8_t, 96> gbufferParams{};
    /// @note paramsBuffer を同じ G-buffer 定数列として直接使える場合に true。
    bool directGBufferParams = false;
    bool valid = false;
    int32_t renderQueue = 2000;
    bool doubleSided = false;
    int32_t depthBias = 0;
    float depthBiasSlope = 0.0f;
};

} // namespace fbzz::renderer
