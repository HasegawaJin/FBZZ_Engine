/// @file    FluidAssetWriters.hpp
/// @brief   焼いたフリップブックを .mat と単層・複数層の .vfx へ書き出す。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note .fluid の Inspector (2D)・Volume Flipbook Baker パネル (3D)・FluidBakeService (AI) が
/// @note それぞれ .mat を組むと同じ焼き結果から違う .mat ができるため、書き方はここだけが知る。
/// @note レンダラーにも AssetManager にも触らない純粋なファイル書き出しなので、テストから直接呼べる。
#pragma once

#include <Engine/Asset/FluidBaker.hpp>
#include <Fluid/FluidRecipe.hpp>
#include <Engine/Asset/VolumeFlipbookBaker.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <span>
#include <vector>

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::editor {

enum class FluidEffectTemplate : std::uint8_t { LANDING, CHARGE_RELEASE, MAGIC_ERUPTION, COUNT };

struct FluidEffectLayer {
    std::string name;
    fluid::FluidRecipe recipe;
    float startDelay = 0.0f;
    float size = 2.0f;
    math::Vector3 position = { 0.0f, 1.0f, 0.0f };
};

[[nodiscard]] const char* FluidEffectTemplateName(FluidEffectTemplate preset);
[[nodiscard]] std::vector<FluidEffectLayer> MakeFluidEffectLayers(FluidEffectTemplate preset);
/// @brief materialPaths は各層の .mat の実パス。同数かつ全て実在すること。既存の .vfx は上書きしない。
[[nodiscard]] bool WriteLayeredFluidVfx(const std::filesystem::path& file, const std::string& rootName,
    std::span<const FluidEffectLayer> layers, std::span<const std::string> materialPaths, std::string& outError);

/// @brief 何を焼いたか。2D (BakeFluid) と 3D (Volume Flipbook Baker) で .mat の組み方が違う。
struct FluidMaterialSource {
    enum class Kind : std::uint8_t { Flat2D, Volume3D };

    Kind kind = Kind::Flat2D;
    asset::FluidBakeResult flat;
    asset::VolumeFlipbookBakeResult volume;
    /// @brief 3D の Atlas を FPS モードでループ再生するか (VolumeBakeLoops)。false なら Lifetime。
    bool volumeLoops = false;
    float volumeFramesPerSecond = 24.0f;
    /// @brief 3D の Atlas が色ではなく歪みマップ (settings.distortion)。2D の Distortion と同じ組み方にする。
    bool volumeDistortion = false;
    /// @brief 3D の Fluid Fire。6-way の発光を煙の alpha から独立させる。
    bool volumeFireEmission = false;
    /// @brief 3D の Fluid Glow。加算合成で描く。
    bool volumeGlow = false;

    [[nodiscard]] static FluidMaterialSource FromFlat(const asset::FluidBakeResult& result);
    [[nodiscard]] static FluidMaterialSource FromVolume(const asset::VolumeFlipbookBakeResult& result,
                                                        const asset::VolumeFlipbookBakeSettings& settings);
};

/// @brief .fluid の隣の同名 .mat (`<dir>/<stem>.mat`) の実パス。
[[nodiscard]] std::string SiblingMaterialPath(const std::string& fluidPath);

/// @brief 新しく作るときの Particle 用 .mat の既定 (両面・深度書き込みなし・透過キュー)。
[[nodiscard]] asset::MaterialAsset NewFluidParticleMaterial();

/// @brief 焼いた結果を Particle マテリアルへ写し、テクスチャ参照は GUID で保存する。
/// @note シェーダーや他の [particle] の値は残す (人が調整した値を焼き直しで消さない)。
/// @return 既存テクスチャの GUID を解決できない場合は false を返し、material は保存しないこと。
[[nodiscard]] bool ApplyFluidBakeToMaterial(const FluidMaterialSource& source, asset::MaterialAsset& material,
                                            std::string& outError);

/// @brief materialPath の .mat を読み (無ければ NewFluidParticleMaterial から作り)、焼いた結果を写して保存する。
/// @param outCreated 新しく作ったなら true
/// @return 読めない既存ファイルや書き込み失敗は false (outError に理由)
[[nodiscard]] bool WriteFluidParticleMaterial(const std::string& materialPath, const FluidMaterialSource& source,
                                              bool& outCreated, std::string& outError);

/// @brief ループするエミッター 1 つだけの .vfx。形式は既存の .vfx (プレハブ TOML) と同じ。
/// @note 1 粒が寿命いっぱいでアトラスを最後まで再生する (Lifetime モードの .mat を想定)。
/// @note PrefabSerializer は実在の Scene から保存するため、使うと編集中のシーンへ一時的な
/// @note GameObject を足して消すことになる (Undo 履歴と «変更あり» 表示を汚すため手で書く)。
/// @param materialAssetPath ParticleEmitter の materialPath にそのまま書く値 (Assets 相対)
[[nodiscard]] bool WriteSingleEmitterVfx(const std::filesystem::path& file, const std::string& rootName,
                                         const std::string& materialAssetPath, float lifetime);

}
