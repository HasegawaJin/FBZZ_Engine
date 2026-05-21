// FBZZ Engine
// ShaderManager.hpp | fbzz::renderer
// IShader のロードとキャッシュ管理
#pragma once

#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <memory>
#include <string>
#include <unordered_map>

namespace fbzz::renderer {

class ShaderManager {
public:
    static void Init(IRenderer* renderer);
    static void Shutdown();

    // path: "assets/shaders/Unlit.hlsl" 形式。2回目以降はキャッシュを返す
    static std::shared_ptr<IShader> Load(const std::string& path);

    static void ClearCache();
    static void Reload(const std::string& path);

private:
    static IRenderer*                                             m_renderer;
    static std::unordered_map<std::string, std::shared_ptr<IShader>> m_cache;
};

} // namespace fbzz::renderer
