// FBZZ Engine
// TextureAsset.hpp | fbzz::asset
// テクスチャアセットのランタイム表現とインポート設定
// .tex TOML descriptor または .png/.dds/.tga 直参照を統一型で扱う。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

enum class TextureType        { Color, Normal, Data, HDR, UI, Sprite };
enum class TextureCompression { Auto, BC1, BC3, BC4, BC5, BC6H, BC7, None };
enum class TextureFilter      { Point, Bilinear, Trilinear, Anisotropic };
enum class TextureWrap        { Repeat, Clamp, Mirror, Border };
enum class MipFilter          { Box, Kaiser, Lanczos };
enum class AlphaMode          { Straight, Premultiplied, None };
enum class CompQuality        { Fast, Normal, High };
enum class SpriteMode         { Single, Multiple };

// Texture内の矩形を名前付きサブアセットとして公開する。
// width/heightが0のSingle Spriteは画像全体を表し、元画像サイズへの依存をメタから排除する。
struct SpriteRect {
    // Sprite 名を変更しても参照を維持する永続 ID。
    // WHY: 表示名を参照キーにすると、Sprite Editor でのリネームが Scene / Material の参照切れになる。
    std::string id;
    std::string name;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    float pivotX = 0.5f;
    float pivotY = 0.5f;
    float borderLeft = 0.0f;
    float borderTop = 0.0f;
    float borderRight = 0.0f;
    float borderBottom = 0.0f;

    bool operator==(const SpriteRect&) const = default;
};

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
    SpriteMode         spriteMode        = SpriteMode::Single;
    float              pixelsPerUnit     = 100.0f;
    std::vector<SpriteRect> sprites;

    bool operator==(const TextureImportSettings&) const = default;
};

// type からデフォルト設定を返す。.tex ローダーはこれを基にして TOML 値で上書きする。
[[nodiscard]] TextureImportSettings DefaultSettingsForType(TextureType type);

// テクスチャ型のファイルパスから TextureType をヒューリスティックで推定する。
// _n.png → Normal, _d.png → Color 等。
[[nodiscard]] TextureType GuessTextureType(std::string_view filename);

// Spriteサブアセット参照は元Textureパスを壊さない文字列形式で保持する。
// 例: "Assets/UI/Atlas.png::sprite::sprite-<stable-id>"
[[nodiscard]] std::string MakeSpriteReference(
    std::string_view texturePath, std::string_view spriteId);
[[nodiscard]] bool ParseSpriteReference(
    std::string_view reference, std::string& outTexturePath, std::string& outSpriteName);
[[nodiscard]] const SpriteRect* FindSprite(
    const TextureImportSettings& settings, std::string_view spriteId);

struct TextureAsset {
    // .tex なら source フィールドが指す画像パス、直参照なら自身のパス
    std::string                                    sourcePath;
    TextureImportSettings                          settings;
    renderer::ResourceHandle<renderer::TextureTag> gpuHandle; // ロード後に設定
};

} // namespace fbzz::asset
