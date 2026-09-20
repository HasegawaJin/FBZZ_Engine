/// @file    BundledModel.hpp
/// @brief   .fzasset (ModelAsset) から Model を組み立てる内部関数の宣言。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#pragma once
#include <memory>

namespace fbzz::asset {

struct Model;
struct ModelAsset;

namespace detail {

/// @brief LOD0 のメッシュを移して Model を組み立てる。GPU には触らない (メッシュのバッファはそのまま移る)。
/// @return asset が空、または LOD を持たなければ nullptr。
/// @note 同期 importer と非同期の Model 経路が同じ組み立てを通るために公開する。ワーカーから呼んでよい。
[[nodiscard]] std::unique_ptr<Model> BuildBundledModel(std::unique_ptr<ModelAsset> asset);

} // namespace detail
} // namespace fbzz::asset
