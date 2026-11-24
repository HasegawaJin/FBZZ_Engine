/// @file    TextureStreamCache.hpp
/// @brief   テクスチャの品質段を独立したチャンクに分けて持つキャッシュ (.fztc)。
/// @author  Hasegawa Jin
/// @date    2026-09-19

/// @note 元画像 (PNG 等) は索引を持たず、低い品質段だけが要るときも全体を展開するしかない。
/// @note 一度展開した結果を品質段ごとのチャンクへ書いておき、以後は要るチャンクだけを読む。
/// @see Docs/design/asset-streaming.md «常駐予算とストリーミング»
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/Streaming/AssetStreaming.hpp>
#include <Engine/Format/FzChunkFormat.hpp>
#include <Graphics/Renderer/Resources/Textures/TextureFileDecoder.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset::texturecache {

/// @note .fztc の中身の種類。
inline constexpr uint32_t kContentKind = MakeFourCC('T', 'X', 'Q', 'L');
/// @note 付帯情報チャンクの ID。品質段のチャンクは 0 から番号を振る。
inline constexpr uint32_t kMetaChunkId = 0xFFFFFFFFu;
/// @note 展開規則の版。展開や縮小のやり方を変えたら上げる (古いキャッシュを捨てさせる)。
inline constexpr uint32_t kDecodeRulesVersion = 2;

/// @brief 元画像の大きさ・更新時刻・ミップ生成設定から世代を作る。どれかが変われば別の値になる。
[[nodiscard]] FBZZ_ENGINE_API uint64_t MakeSourceStamp(uint64_t sourceSize, int64_t sourceWriteTime,
    const renderer::TextureMipSettings& settings = {});

/// @brief 元画像パスからキャッシュのファイル名 (拡張子込み) を作る。大小と区切り文字の違いは同じ名前になる。
[[nodiscard]] FBZZ_ENGINE_API std::string CacheFileName(const std::string& sourcePath);
/// @brief 配布先の元画像の隣に品質キャッシュを生成する。対応外の拡張子・ネイティブ DDS は何もせず成功する。
/// @note 出力先で世代を計算するため、開発 PC の絶対パスへ依存しない。
/// @param settingsSourcePath 空でなければコピー元の画像パス。meta のコピー順に依存せず生成設定を取得する。
[[nodiscard]] FBZZ_ENGINE_API bool BakeDistributionTexture(const std::string& sourcePath, std::string& outError,
    const std::string& settingsSourcePath = {});

/// @brief 全段展開した画像から、品質段ごとの表現を作る。
/// @note ミップ付きの画像は «q 段目以降のミップ列»、1 段の画像は «q 回縮小した 1 段» が品質段 q の表現。
/// @note 自動ミップ生成を行う画像は PrepareTextureMipmaps の結果を渡し、同期経路と同じ列から選ぶ。
[[nodiscard]] FBZZ_ENGINE_API renderer::DecodedTextureRGBA8 SelectQuality(const renderer::DecodedTextureRGBA8& full, AssetQuality quality);

/// @brief 全品質段 [0, lowest] を書き出す。書けなくても読み込みは続けてよい (キャッシュは任意)。
[[nodiscard]] FBZZ_ENGINE_API bool Write(const std::string& cachePath, uint64_t sourceStamp,
                         const renderer::DecodedTextureRGBA8& full, AssetQuality lowest);

/// @brief 品質段 quality の表現だけをキャッシュから読む。
/// @return キャッシュが無い・古い・壊れている・その段を持たないなら false。
/// @param outBytesRead 実際にディスクから読んだバイト数 (null 可)。
[[nodiscard]] FBZZ_ENGINE_API bool ReadQuality(const std::string& cachePath, uint64_t sourceStamp, AssetQuality quality,
                               renderer::DecodedTextureRGBA8& out, uint64_t* outBytesRead = nullptr);

} /// @note namespace fbzz::asset::texturecache
