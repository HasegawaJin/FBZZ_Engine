/// @file    SkinningPolicy.hpp
/// @brief   Graphics が解決した Skinning 実行方式の問い合わせ。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Api.hpp>
namespace fbzz::renderer {
[[nodiscard]] FBZZ_GRAPHICS_API bool UseOwnedSkinningBatch();
} /// @note namespace fbzz::renderer
