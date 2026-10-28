/// @file    OwnedSkinningBatch.hpp
/// @brief   Private selection of the synchronous owned skinning batch and its legacy control.
/// @author  Hasegawa Jin
/// @date    2026-10-14
#pragma once
#include <cstdlib>
#include <cstring>

namespace fbzz::renderer::skinning_batch_detail {
/// @note The owned batch is the default; explicit 0 keeps control and explicit 1 selects owned work.
/// @note An explicitly enabled GPU-demand experiment keeps its original control path unless owned work is explicitly requested.
[[nodiscard]] inline bool Enabled()
{
    const char* value = std::getenv("FBZZ_OWNED_SKINNING_BATCH");
    if (value) return std::strcmp(value, "1") == 0;
    const char* demand = std::getenv("FBZZ_GPU_SKINNING_DEMAND");
    return !demand || std::strcmp(demand, "1") != 0;
}
} /// @note namespace fbzz::renderer::skinning_batch_detail
