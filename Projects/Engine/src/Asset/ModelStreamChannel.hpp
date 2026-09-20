/// @file    ModelStreamChannel.hpp
/// @brief   Model / ModelAsset / MaterialAsset の非同期経路 (ワーカーで解析、メインで頂点バッファを作る)。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <Engine/Asset/AssetStreaming.hpp>
#include <memory>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::asset {

/// @brief Model (シーンが描く LOD0 の束) の経路。
/// @note 公開中の Model は差し替えない。MeshRenderer が Mesh* を raw pointer で持つため、中身を
///       入れ替えると描画側が解放済みのメッシュを指す。読み直しは同期の ReloadPath / 再インポートに任せる。
[[nodiscard]] std::shared_ptr<IAssetStreamChannel> CreateModelStreamChannel(renderer::ResourceManager& resources);

/// @brief ModelAsset (LOD 付き) の経路。品質段 n は LOD n より高品質なメッシュを常駐させない。
[[nodiscard]] std::shared_ptr<IAssetStreamChannel> CreateModelAssetStreamChannel(renderer::ResourceManager& resources);

/// @brief MaterialAsset (.mat) の経路。参照するテクスチャを任意依存として一緒に要求する。
/// @note 任意依存なので、テクスチャが欠けても .mat 自体は完成する (描画側はテクスチャ無しで描く)。
[[nodiscard]] std::shared_ptr<IAssetStreamChannel> CreateMaterialStreamChannel();

} // namespace fbzz::asset
