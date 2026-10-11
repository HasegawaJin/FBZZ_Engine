/// @file    AssetBrowserThumbnail.hpp
/// @brief   Bounded CPU thumbnail decoding and original asset-type icon selection.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fbzz::editor::assetthumbnail {

enum class Icon : uint8_t { SCRIPT, SCENE, PREFAB, AUDIO, ANIMATION, SHADER, COUNT, NONE = COUNT };
enum class State : uint8_t { PENDING, READY, FAILED, UNSUPPORTED };

inline constexpr uint64_t MAX_FILE_BYTES = 32ull * 1024 * 1024;
inline constexpr uint64_t MAX_PIXELS = 16ull * 1024 * 1024;
inline constexpr uint32_t MAX_EDGE = 256;
inline constexpr size_t MAX_CACHE_ENTRIES = 192;

[[nodiscard]] constexpr Icon TypeIcon(std::string_view extension)
{
    if (extension == ".cs" || extension == ".py" || extension == ".lua" ||
        extension == ".cpp" || extension == ".hpp" || extension == ".c" ||
        extension == ".h" || extension == ".cc" || extension == ".cxx") return Icon::SCRIPT;
    if (extension == ".scene") return Icon::SCENE;
    if (extension == ".prefab") return Icon::PREFAB;
    if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" ||
        extension == ".flac" || extension == ".synth") return Icon::AUDIO;
    if (extension == ".anim" || extension == ".animcontroller" || extension == ".animctrl" ||
        extension == ".mask" || extension == ".sequence") return Icon::ANIMATION;
    if (extension == ".hlsl" || extension == ".hlsli") return Icon::SHADER;
    return Icon::NONE;
}

[[nodiscard]] constexpr const char* IconName(Icon icon)
{
    switch (icon) {
    case Icon::SCRIPT: return "Script";
    case Icon::SCENE: return "Scene";
    case Icon::PREFAB: return "Prefab";
    case Icon::AUDIO: return "Audio";
    case Icon::ANIMATION: return "Animation";
    case Icon::SHADER: return "Shader";
    default: return "";
    }
}

struct Image {
    State state = State::FAILED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    std::vector<uint8_t> rgba;
};

/// @note CPU only; the caller creates and releases GPU resources on the renderer thread.
/// @note Encoded file and decoded pixel limits are checked before allocation. Cubemap/array/volume DDS are unsupported.
[[nodiscard]] Image Decode(const std::filesystem::path& path);

} /// @note namespace fbzz::editor::assetthumbnail
