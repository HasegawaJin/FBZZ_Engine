/// @file    ParticleMaterialSettings.cpp
/// @brief   materialPath へテクスチャが入ったときの «決まった移行先» を決める。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/ParticleMaterialSettings.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace fbzz::asset {
namespace {

// Particle 用 .mat の置き場。テンプレートが同梱している 25 枚もここにある。
constexpr const char* kParticleMaterialDir = "Assets/Materials/Particles";

constexpr std::array<std::string_view, 5> kTextureExtensions = {
    ".png", ".tga", ".dds", ".jpg", ".jpeg"
};

[[nodiscard]] std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// ファイル名に使う短い識別子。.mat 本文の正式名 ("AlphaBlend") とは別で、
// ファイル名は既存アセットと揃う短い名 ("Alpha") を使う。
[[nodiscard]] const char* BlendFileSuffix(scene::ParticleBlendMode blend)
{
    switch (blend) {
    case scene::ParticleBlendMode::Alpha:         return "Alpha";
    case scene::ParticleBlendMode::Premultiplied: return "Premultiplied";
    default:                               return "Additive";
    }
}

// "flame_03" -> "Flame03"。区切りを落として各語の頭を大文字にするだけの決定的な変換。
// WHY: 同梱の 25 枚と同じ名前へ落ちることで、テンプレートが既に使っている .mat を
//      作り直さずそのまま再利用できる。命名がぶれると同じ素材の .mat が二重に増える。
[[nodiscard]] std::string ToMaterialStem(std::string_view textureStem)
{
    std::string result;
    result.reserve(textureStem.size());
    bool atWordStart = true;
    for (const char c : textureStem) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            atWordStart = true;
            continue;
        }
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc)) continue;
        result.push_back(atWordStart ? static_cast<char>(std::toupper(uc)) : c);
        atWordStart = false;
    }
    return result;
}

[[nodiscard]] std::string_view Extension(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot   = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    if (slash != std::string_view::npos && dot < slash) return {};
    return path.substr(dot);
}

[[nodiscard]] std::string_view Stem(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

} // namespace

bool IsParticleTexturePath(std::string_view path)
{
    const std::string ext = ToLower(std::string(Extension(path)));
    return std::find(kTextureExtensions.begin(), kTextureExtensions.end(), ext)
        != kTextureExtensions.end();
}

std::string ParticleMaterialPathForTexture(std::string_view texturePath, scene::ParticleBlendMode blend)
{
    if (texturePath.empty()) return {};
    const std::string stem = ToMaterialStem(Stem(texturePath));
    if (stem.empty()) return {};
    return std::string(kParticleMaterialDir) + "/" + stem + "_" + BlendFileSuffix(blend) + ".mat";
}

} // namespace fbzz::asset
