// FBZZ Engine
// ScriptMaterialProxy.hpp | fbzz::scene
// Script から MaterialComponent と RenderPass を操作するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <string_view>

namespace fbzz::renderer {
struct ShaderDescriptor;
}

namespace fbzz::scene {

class Script;
struct UserRenderPassDesc;

struct ScriptMaterialProxy {
    Script* script = nullptr;

    void SetFloat(std::string_view param, float v) const;
    void SetInt(std::string_view param, int v) const;
    void SetVector3(std::string_view param, const math::Vector3& v) const;
    void SetVector4(std::string_view param, const math::Vector4& v) const;
    void SetTexture(std::string_view slot, std::string_view texPath) const;
    void QueueRenderPass(UserRenderPassDesc desc) const;
    const renderer::ShaderDescriptor* GetShaderDescriptor(std::string_view path) const;
};

} // namespace fbzz::scene
