// FBZZ Engine
// ShaderManager.cpp | fbzz::renderer
// IShader のロードとキャッシュ管理
#include <engine/Renderer/ShaderManager.hpp>
#include <engine/Core/Logger.hpp>
#include <cassert>

namespace fbzz::renderer {

IRenderer*                                             ShaderManager::m_renderer = nullptr;
std::unordered_map<std::string, std::shared_ptr<IShader>> ShaderManager::m_cache;

void ShaderManager::Init(IRenderer* renderer) {
    assert(renderer && "ShaderManager::Init に nullptr が渡された");
    m_renderer = renderer;
}

void ShaderManager::Shutdown() {
    m_cache.clear();
    m_renderer = nullptr;
}

std::shared_ptr<IShader> ShaderManager::Load(const std::string& path) {
    assert(m_renderer && "ShaderManager::Init を先に呼ぶこと");

    auto it = m_cache.find(path);
    if (it != m_cache.end()) return it->second;

    auto shader = m_renderer->CreateShader(path);
    if (!shader) {
        FBZZ_LOG_ERROR("シェーダーのロードに失敗: %s", path.c_str());
        return nullptr;
    }

    m_cache[path] = shader;
    return shader;
}

void ShaderManager::ClearCache() {
    m_cache.clear();
}

void ShaderManager::Reload(const std::string& path) {
    m_cache.erase(path);
    Load(path);
}

} // namespace fbzz::renderer
