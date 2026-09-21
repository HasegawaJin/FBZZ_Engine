/// @file    ShaderPathResolver.cpp
/// @brief   プロジェクトを知らないシェーダーパス解決。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Renderer/ShaderPathResolver.hpp>
namespace fbzz::renderer {
namespace { ShaderPathResolver g_resolver = nullptr; }
void SetShaderPathResolver(ShaderPathResolver resolver) { g_resolver = resolver; }
std::filesystem::path ResolveShaderFilePath(const std::filesystem::path& requested)
{
    return g_resolver ? g_resolver(requested) : requested;
}
}
