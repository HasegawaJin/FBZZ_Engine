// FBZZ Engine
// TextureAsset.hpp | fbzz::asset
// テクスチャアセットのランタイム表現とインポート設定
// .tex TOML descriptor または .png/.dds/.tga 直参照を統一型で扱う。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <string>

namespace fbzz::asset {

enum class TextureType        { Color, Normal, Data, HDR, UI };
enum class TextureCompression { Auto, BC1, BC3, BC4, BC5, BC6H, BC7, None };
enum class TextureFilter      { Point, Bilinear, Trilinear, Anisotropic };
enum class TextureWrap        { Repeat, Clamp, Mirror, Border };
enum class MipFilter          { Box, Kaiser, Lanczos };
enum class AlphaMode          { Straight, Premultiplied, None };
enum class CompQuality        { Fast, Normal, High };

struct TextureImportSettings {
    TextureType        type               = TextureType::Color;
    TextureCompression compression        = TextureCompression::Auto;
    CompQuality        compressionQuality = CompQuality::Normal;
    bool               srgb              = true;
    bool               mipmaps           = true;
    MipFilter          mipFilter         = MipFilter::Kaiser;
    float              mipSharpen        = 0.0f;
    float              mipBias           = 0.0f;
    bool               normalizeMipmaps  = false;
    bool               flipGreen         = false;
    uint32_t           maxSize           = 4096;
    TextureWrap        wrapU             = TextureWrap::Repeat;
    TextureWrap        wrapV             = TextureWrap::Repeat;
    TextureFilter      filter            = TextureFilter::Trilinear;
    uint32_t           anisoLevel        = 4;
    AlphaMode          alphaMode         = AlphaMode::Straight;
    bool               alphaDither       = false;
};

// type からデフォルト設定を返す。.tex ローダーはこれを基にして TOML 値で上書きする。
[[nodiscard]] TextureImportSettings DefaultSettingsForType(TextureType type);

// テクスチャ型のファイルパスから TextureType をヒューリスティックで推定する。
// _n.png → Normal, _d.png → Color 等。
[[nodiscard]] TextureType GuessTextureType(std::string_view filename);

struct TextureAsset {
    // .tex なら source フィールドが指す画像パス、直参照なら自身のパス
    std::string                                    sourcePath;
    TextureImportSettings                          settings;
    renderer::ResourceHandle<renderer::TextureTag> gpuHandle; // ロード後に設定
};

} // namespace fbzz::asset
