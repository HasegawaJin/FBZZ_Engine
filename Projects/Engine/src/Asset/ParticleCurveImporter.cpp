/// @file    ParticleCurveImporter.cpp
/// @brief   .curve / .gradient の読み込み。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/ParticleCurveImporter.hpp>

namespace fbzz::asset {

std::unique_ptr<ParticleCurveAsset> ParticleCurveImporter::Import(
    const std::string& absPath, renderer::ResourceManager* resources)
{
    (void)resources; // 曲線は GPU 資源を持たない
    auto asset = std::make_unique<ParticleCurveAsset>();
    if (!LoadParticleCurveAssetFile(absPath, *asset)) return nullptr;
    return asset;
}

} // namespace fbzz::asset
