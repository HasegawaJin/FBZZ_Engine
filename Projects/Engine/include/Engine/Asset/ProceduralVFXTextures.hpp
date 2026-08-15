// FBZZ Engine
// ProceduralVFXTextures.hpp | fbzz::asset
// VFX向けFlipbookとImpact DecalテクスチャのCPUプロシージャル生成API
#pragma once

#include <cstdint>
#include <string>

namespace fbzz::asset {

// Particle.hlsl が通常RGBAとして読めるFlipbookプリセット。
// DistortionだけはRGを符号付き変位、Aを効果範囲として生成する。
enum class ProceduralFlipbookPreset : std::uint8_t {
    Smoke = 0,
    Fire,
    Explosion,
    Distortion,
};

// 1行を1本のアニメーションとして生成する。
// WHY: ParticleEmitter::spriteRandomRow と一致させ、行ごとに見た目の異なる粒子を選べるようにする。
struct ProceduralFlipbookSettings {
    ProceduralFlipbookPreset preset = ProceduralFlipbookPreset::Smoke;
    int frameSize = 128;
    int columns = 8;
    int rows = 2;
    std::uint32_t seed = 1;
    float noiseScale = 4.0f;
    float warpStrength = 0.65f;
};

struct ProceduralImpactDecalSettings {
    int textureSize = 512;
    std::uint32_t seed = 1;
    float radius = 0.72f;
    int crackCount = 12;
    float emissiveStrength = 0.65f;
};

// 生成結果は実ファイルパスを返す。Editor側でAssets起点パスへ正規化してアセットへ保存する。
struct ProceduralVFXTextureResult {
    bool success = false;
    std::string message;
    std::string albedoPath;
    std::string normalPath;
    std::string emissivePath;
};

// outputDirectoryへ衝突しない連番名でPNGを書き出す。
[[nodiscard]] ProceduralVFXTextureResult GenerateProceduralFlipbook(
    const std::string& outputDirectory, const ProceduralFlipbookSettings& settings);

// Albedo / Normal / Emissiveの3枚を同じベース名で書き出す。
[[nodiscard]] ProceduralVFXTextureResult GenerateProceduralImpactDecal(
    const std::string& outputDirectory, const ProceduralImpactDecalSettings& settings);

[[nodiscard]] const char* ProceduralFlipbookPresetName(ProceduralFlipbookPreset preset);

} // namespace fbzz::asset
