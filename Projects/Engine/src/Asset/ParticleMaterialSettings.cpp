/// @file    ParticleMaterialSettings.cpp
/// @brief   .mat の [particle] に付随する規則 — テクスチャを包む .mat の名前とフリップブックのコマ評価
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Engine/Asset/ParticleMaterialSettings.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

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

namespace {

[[nodiscard]] float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

} // namespace

FlipbookFrameRange ResolveFlipbookRange(const ParticleFlipbookSettings& flipbook)
{
    FlipbookFrameRange range;
    range.columns = (std::max)(flipbook.spriteColumns, 1);
    range.rows    = (std::max)(flipbook.spriteRows, 1);
    const int frameCount = range.columns * range.rows;
    range.first = std::clamp(flipbook.spriteStartFrame, 0, frameCount - 1);
    range.last  = std::clamp(flipbook.spriteEndFrame > 0 ? flipbook.spriteEndFrame : frameCount - 1,
                             range.first, frameCount - 1);
    return range;
}

FlipbookFrameSample EvaluateFlipbookFrame(const ParticleFlipbookSettings& flipbook,
                                          float normalizedAge, float ageSeconds, float spriteSeed)
{
    const FlipbookFrameRange range = ResolveFlipbookRange(flipbook);
    int startFrame = range.first;
    int endFrame   = range.last;
    // Random Row: アトラスの各行を «1 本のアニメーションのバリエーション» として扱い、
    // 粒子ごとに 1 行を選んでその中だけで再生する。1 枚のアトラスで見た目の異なる
    // 煙・爆炎を混ぜられる (AAA のアトラスはこの構成が標準)。
    if (flipbook.spriteRandomRow && range.rows > 1) {
        const int row = std::clamp(
            static_cast<int>(Clamp01(spriteSeed) * static_cast<float>(range.rows)), 0, range.rows - 1);
        startFrame = row * range.columns;
        endFrame   = startFrame + range.columns - 1;
    }
    const int   span = (std::max)(endFrame - startFrame, 0);
    const float fps  = (std::max)(flipbook.flipbookFramesPerSecond, 0.0f);
    float framePosition = 0.0f;
    bool  wrapNext      = false;
    switch (flipbook.flipbookMode) {
    case scene::ParticleFlipbookMode::FramesPerSecond:
        framePosition = span > 0 ? std::fmod(ageSeconds * fps, static_cast<float>(span + 1)) : 0.0f;
        wrapNext = true;
        break;
    case scene::ParticleFlipbookMode::RandomFrame:
        framePosition = std::floor(Clamp01(spriteSeed) * static_cast<float>(span));
        break;
    case scene::ParticleFlipbookMode::PingPong: {
        const float cycleLength = static_cast<float>((std::max)(span * 2, 1));
        const float cycleFrame  = std::fmod(ageSeconds * fps, cycleLength);
        framePosition = cycleFrame <= static_cast<float>(span)
            ? cycleFrame : static_cast<float>(span * 2) - cycleFrame;
        break;
    }
    case scene::ParticleFlipbookMode::Lifetime:
    default:
        framePosition = Clamp01(normalizedAge) * static_cast<float>(span);
        break;
    }
    // Random Start Frame: 再生位相を粒子ごとにずらす。
    // これが無いと同時に湧いた煙が全部同じコマで回り、群れが一枚の板に見えてしまう。
    // RandomFrame はコマ自体がランダムなので位相ずらしは適用しない。
    if (flipbook.spriteRandomStartFrame && span > 0
        && flipbook.flipbookMode != scene::ParticleFlipbookMode::RandomFrame) {
        // 行選択と同じ seed をそのまま使うと «行と位相» が相関して規則性が見えるため、
        // 係数でずらしてから小数部を取り、独立した第 2 の乱数として扱う。
        const float phaseSeed    = Clamp01(spriteSeed) * 7.13f + 0.37f;
        const float decorrelated = phaseSeed - std::floor(phaseSeed);
        const float cycle        = static_cast<float>(span + 1);
        framePosition = std::fmod(framePosition + std::floor(decorrelated * cycle), cycle);
        wrapNext = true;
    }
    const int relativeFrame = std::clamp(static_cast<int>(std::floor(framePosition)), 0, span);
    int nextRelativeFrame   = (std::min)(relativeFrame + 1, span);
    if (wrapNext && relativeFrame == span) nextRelativeFrame = 0;

    FlipbookFrameSample sample;
    sample.frame     = startFrame + relativeFrame;
    sample.nextFrame = startFrame + nextRelativeFrame;
    sample.blend     = flipbook.flipbookFrameBlending
                           && flipbook.flipbookMode != scene::ParticleFlipbookMode::RandomFrame
        ? framePosition - std::floor(framePosition) : 0.0f;
    return sample;
}

} // namespace fbzz::asset
