/// @file    DX12UploadArenaSizing.hpp
/// @brief   Bounded startup sizing for the diagnostic DX12 upload arena override.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once

#include <cstddef>
#include <string_view>

namespace fbzz::renderer::upload_arena_detail {

inline constexpr size_t MIB = 1024u * 1024u;
inline constexpr size_t DEFAULT_BYTES_PER_FRAME = 16u * MIB;

struct UploadArenaSizing {
    size_t bytesPerFrame = DEFAULT_BYTES_PER_FRAME;
    bool valid = true;
    bool specified = false;
};

/// @note The override accepts exact MiB tokens only; absent input preserves 16 MiB without a warning.
/// @return Invalid explicit input selects the safe default and reports valid=false without numeric conversion.
[[nodiscard]] constexpr UploadArenaSizing ParseUploadArenaMiB(std::string_view value, bool specified)
{
    if (!specified) return {};
    if (value == "16") return {16u * MIB, true, true};
    if (value == "32") return {32u * MIB, true, true};
    if (value == "64") return {64u * MIB, true, true};
    if (value == "128") return {128u * MIB, true, true};
    return {DEFAULT_BYTES_PER_FRAME, false, true};
}

} /// @note namespace fbzz::renderer::upload_arena_detail
