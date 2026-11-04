/// @file    FlipbookImageIO.hpp
/// @brief   生成系アセット (Flipbook / MV / 手続きテクスチャ) が共有する PNG と .meta の書き出し。
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note DirectXTex に依存する実装詳細で、Engine の公開 API にする理由が無いため src 内に閉じる。
/// @note 同じ処理が生成器ごとの無名名前空間に 3 通り重複していたため、ここへ寄せた。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/FlipbookMips.hpp>
#include <Engine/Asset/TextureAsset.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace fbzz::asset::detail {

/// @note baseName, baseName_001, ... の順に、どの suffix とも衝突しないベースパスを探す。
/// @note 見つからなければ空のパスを返す。
[[nodiscard]] FBZZ_ENGINE_API std::filesystem::path FindAvailableBase(
    const std::filesystem::path& directory, std::string_view baseName,
    std::initializer_list<std::string_view> suffixes);

/// @note rowPitch = 0 は width * 4 (詰めて並んでいる) とみなす。
[[nodiscard]] FBZZ_ENGINE_API bool SavePngRgba8(const std::filesystem::path& path, std::uint32_t width,
                                std::uint32_t height, std::span<const std::uint8_t> pixels,
                                std::string& outError, std::size_t rowPitch = 0);

/// @note mip を持たないテクスチャは Clamp / Bilinear にする (Atlas では隣のコマが滲むため)。
/// @note Data / Normal は sRGB 変換を必ず切る。
[[nodiscard]] FBZZ_ENGINE_API bool SaveTextureMeta(const std::filesystem::path& sourcePath, TextureType type,
                                   TextureCompression compression, AlphaMode alphaMode,
                                   bool mipmaps, std::string& outError);

enum class FlipbookDdsCompression : std::uint8_t { BC7, BC5, None };

/// @note コマを跨がないミップ (BuildFlipbookMips) 付きの DDS。PNG は 1 段しか読まれないので、
/// @note 実行時に使うフリップブックはこちらを参照させる。BC7 は «速い» モードで圧縮する。
/// @note 0 段目の寸法が 4 の倍数でなければ BC 圧縮はできないので無圧縮で書く。
[[nodiscard]] FBZZ_ENGINE_API bool SaveFlipbookDds(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
                                   std::span<const std::uint8_t> pixels, std::uint32_t tileWidth,
                                   std::uint32_t tileHeight, FlipbookMipContent content,
                                   FlipbookDdsCompression compression, std::string& outError,
                                   const std::atomic<bool>* cancel = nullptr);

/// @note ミップ付きフリップブックの .meta。ラップは Clamp (Atlas の端のコマが反対側から滲まないように)。
[[nodiscard]] FBZZ_ENGINE_API bool SaveFlipbookMeta(const std::filesystem::path& sourcePath, TextureType type,
                                    TextureCompression compression, AlphaMode alphaMode, std::string& outError);

} /// @note namespace fbzz::asset::detail
