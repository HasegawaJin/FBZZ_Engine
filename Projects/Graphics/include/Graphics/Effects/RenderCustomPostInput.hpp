/// @file    RenderCustomPostInput.hpp
/// @brief   カスタムポスト処理の解決済み入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Graphics/Renderer/RenderState.hpp>
#include <array>
#include <vector>
namespace fbzz::renderer {
struct RenderCustomPostInput {
    ResourceHandle<ShaderTag> shader;
    ResourceHandle<ConstantBufferTag> paramsBuffer;
    std::array<ResourceHandle<TextureTag>, 5> textures{};
    std::vector<uint8_t> parameters;
    BlendMode blendMode = BlendMode::OPAQUE_BLEND;
    bool IsValid() const { return shader.IsValid(); }
};
}
