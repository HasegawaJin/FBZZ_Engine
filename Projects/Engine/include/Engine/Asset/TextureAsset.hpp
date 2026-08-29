/// @file    TextureAsset.hpp
/// @brief   テクスチャアセットのランタイム表現とインポート設定。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// .tex TOML descriptor または .png/.dds/.tga 直参照を統一型で扱う。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
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

// テクスチャ参照 1 件を、描く側が必要とする数値へ解決した結果。
//
// WHY 型にまとめるか:
//   「参照を割って .meta を読み、矩形を UV へ直す」までを UISystem・SpriteRenderer・
//   Inspector がそれぞれ書いており、幅 0 の Single Sprite が画像全体を指す規則のような
//   細部が写し違いで散っていた。解決の正解を 1 箇所に置き、呼ぶ側は結果だけを見る。
struct ResolvedSprite {
    std::string   texturePath;                  ///< 元画像パス (Sprite 参照でなければ入力そのまま)
    std::string   spriteId;                     ///< Sprite 参照のときだけ非空
    math::Vector2 uvMin      = { 0.0f, 0.0f };
    math::Vector2 uvMax      = { 1.0f, 1.0f };
    math::Vector4 border     = { 0.0f, 0.0f, 0.0f, 0.0f }; ///< 9-slice の余白 L,T,R,B (ピクセル)
    math::Vector2 sizePixels = { 0.0f, 0.0f };  ///< 切り抜きのピクセル寸法 (未解決なら 0)
    math::Vector2 pivot      = { 0.5f, 0.5f };
    float pixelsPerUnit      = 100.0f;
    bool  isSpriteReference  = false;           ///< 入力が "::sprite::" 形式だった
    bool  resolved           = false;           ///< .meta から矩形を取り出せた
};

/// @param textureWidth  元画像の実ピクセル幅。0 以下だと UV へ直せないため矩形は解決しない。
/// @param textureHeight 同・高さ。
[[nodiscard]] ResolvedSprite ResolveSpriteReference(
    std::string_view reference, float textureWidth, float textureHeight);

struct TextureAsset {
    // .tex なら source フィールドが指す画像パス、直参照なら自身のパス
    std::string                                    sourcePath;
    TextureImportSettings                          settings;
    renderer::ResourceHandle<renderer::TextureTag> gpuHandle; // ロード後に設定
};

} // namespace fbzz::asset
