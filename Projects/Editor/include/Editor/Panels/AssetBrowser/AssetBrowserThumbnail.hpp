/// @file    AssetBrowserThumbnail.hpp
/// @brief   サムネイル復号の上限とアセット種別アイコンの選択。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fbzz::editor::assetthumbnail {

enum class Icon : uint8_t { SCRIPT, SCENE, PREFAB, AUDIO, ANIMATION, SHADER, FOLDER, COUNT, NONE = COUNT };
inline constexpr std::array<uint32_t, 6> ICON_SIZES{16, 24, 32, 64, 128, 256};
inline constexpr size_t ICON_CACHE_SIZE = static_cast<size_t>(Icon::COUNT) * ICON_SIZES.size();

enum class State : uint8_t { PENDING, READY, FAILED, UNSUPPORTED };

inline constexpr uint64_t MAX_FILE_BYTES = 32ull * 1024 * 1024;
inline constexpr uint64_t MAX_PIXELS = 16ull * 1024 * 1024;
inline constexpr uint32_t MAX_EDGE = 256;
inline constexpr size_t MAX_CACHE_ENTRIES = 192;

[[nodiscard]] constexpr Icon TypeIcon(std::string_view extension)
{
    if (extension == ".cs" || extension == ".py" || extension == ".lua" ||
        extension == ".cpp" || extension == ".hpp" || extension == ".c" ||
        extension == ".h" || extension == ".cc" || extension == ".cxx") {
        return Icon::SCRIPT;
    }
    if (extension == ".scene") {
        return Icon::SCENE;
    }
    if (extension == ".prefab") {
        return Icon::PREFAB;
    }
    if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" ||
        extension == ".flac" || extension == ".synth") {
        return Icon::AUDIO;
    }
    if (extension == ".anim" || extension == ".animcontroller" || extension == ".animctrl" ||
        extension == ".mask" || extension == ".sequence") {
        return Icon::ANIMATION;
    }
    if (extension == ".hlsl" || extension == ".hlsli") {
        return Icon::SHADER;
    }
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
    case Icon::FOLDER: return "Folder";
    default: return "";
    }
}

struct Image {
    State state = State::FAILED; ///< サムネイル画像の取得結果または未対応の状態。
    uint32_t width = 0; ///< 縮小後の画像の幅 [px]。
    uint32_t height = 0; ///< 縮小後の画像の高さ [px]。
    uint32_t sourceWidth = 0; ///< 元の画像の幅 [px]。
    uint32_t sourceHeight = 0; ///< 元の画像の高さ [px]。
    std::vector<uint8_t> rgba; ///< サムネイル画像の RGBA 8 ビット画素を行優先で保持する。
};

/// @note CPU only; the caller creates and releases GPU resources on the renderer thread.
/// @note Encoded file and decoded pixel limits are checked before allocation. Cubemap/array/volume DDS are unsupported.
[[nodiscard]] Image Decode(const std::filesystem::path& path);

} // namespace fbzz::editor::assetthumbnail
