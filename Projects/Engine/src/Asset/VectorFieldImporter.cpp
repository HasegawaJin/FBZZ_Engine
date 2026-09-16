/// @file    VectorFieldImporter.cpp
/// @brief   拡張子で .vfield / .fga を振り分け、読めたら GPU へ載せる。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/VectorFieldImporter.hpp>

#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cctype>

namespace fbzz::asset {

std::unique_ptr<VectorFieldAsset> VectorFieldImporter::Import(
    const std::string& absPath, renderer::ResourceManager* resources)
{
    std::string extension;
    if (const size_t dot = absPath.find_last_of('.'); dot != std::string::npos)
        extension = absPath.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto field = std::make_unique<VectorFieldAsset>();
    const bool loaded = extension == ".fga" ? ImportFgaFile(absPath, *field)
                                            : LoadVectorFieldFile(absPath, *field);
    if (!loaded) return nullptr;

    // 解像度と量子化はここで必ず揃える。resources の有無に依らないので、
    // 静的検査もテストも実行時と同じ場を見る。GPU への常駐は
    // VelocityFieldAtlas が «実際に使われたとき» に行う。
    (void)resources;
    NormalizeVectorField(*field);
    return field;
}

} // namespace fbzz::asset
