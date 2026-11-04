/// @file    TextureStreamChannel.hpp
/// @brief   TextureAsset の非同期経路 (ワーカーで展開、graphics queue で待たずに転送)。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <memory>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::asset {

/// @brief AssetStreamer へ登録する TextureAsset の経路を作る。
/// @param resources チャンネルより長生きすること (AssetManager::UnloadAll で経路ごと捨てられる)。
/// @note 完成したテクスチャは ResourceManager のパスキャッシュへ載せる。同期 ImageImporter と同じ
/// @note キー (元画像パス) なので、以後の LoadTexture は転送済みの実体を返し二重に読まない。
/// @see Docs/design/asset-streaming.md «GPU 転送と公開»
[[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<IAssetStreamChannel> CreateTextureStreamChannel(renderer::ResourceManager& resources);

} /// @note namespace fbzz::asset
