/// @file    IcoImage.hpp
/// @brief   .ico を RGBA8 へ展開するデコーダー。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @note stb_image は .ico を読めず、DirectXTex (WIC) は読めても先頭フレームしか返さない。.ico は
///       16px〜256px を束ねる形式なので先頭を引くとサムネイルに 16px が出る。ここは面積最大を選ぶ。
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

/// .ico から取り出した 1 枚。rgba は上の行から詰めた 8bit RGBA。
struct IcoImage {
    int                       width      = 0;
    int                       height     = 0;
    int                       frameCount = 0; ///< ファイルに入っていた総フレーム数
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool IsValid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

/// 面積最大のフレームを展開する。
/// @param outError 失敗理由 (UI へそのまま出す)
[[nodiscard]] bool DecodeIcoFile(const std::filesystem::path& path,
                                 IcoImage& out,
                                 std::string& outError);

/// メモリ上の .ico を展開する。
[[nodiscard]] bool DecodeIcoBytes(const std::uint8_t* bytes,
                                  std::size_t size,
                                  IcoImage& out,
                                  std::string& outError);

} // namespace fbzz::editor
