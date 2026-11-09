/// @file    SkinningPolicy.cpp
/// @brief   Skinning の実行方式と環境変数の解決を Graphics に閉じる。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Graphics/Renderer/SkinningPolicy.hpp>
#include "../Passes/Geometry/OwnedSkinningBatch.hpp"
#include <cstdlib>
#include <cstring>
namespace fbzz::renderer {
bool UseOwnedSkinningBatch() { return skinning_batch_detail::Enabled(); }

bool UseSkinningWaitOptimization()
{
    const char* value = std::getenv("FBZZ_SKINNING_WAIT_OPTIMIZATION");
    return !value || std::strcmp(value, "0") != 0;
}
} /// @note namespace fbzz::renderer
