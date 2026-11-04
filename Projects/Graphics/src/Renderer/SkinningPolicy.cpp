/// @file    SkinningPolicy.cpp
/// @brief   Skinning の実行方式と環境変数の解決を Graphics に閉じる。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Graphics/Renderer/SkinningPolicy.hpp>
#include "../Passes/Geometry/OwnedSkinningBatch.hpp"
namespace fbzz::renderer {
bool UseOwnedSkinningBatch() { return skinning_batch_detail::Enabled(); }
} /// @note namespace fbzz::renderer
