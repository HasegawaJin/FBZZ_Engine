/// @file    ImageCompare.hpp
/// @brief   基準画像との比較 (絵の回帰判定) と、差分を赤で強調した画像の生成。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see     Docs/design/ai-verification-loop.md «3. 絵の回帰»
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace fbzz::editor::playtest {

/// @brief 8bit RGBA、左上原点、行優先。
struct RgbaImage {
    uint32_t             width = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> pixels;

    [[nodiscard]] bool IsValid() const { return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * height * 4; }
};

struct ImageDiffSettings {
    /// @brief 1 画素の差 (RGB の最大チャンネル差、0..1) がこれを越えたら «違う画素» と数える。
    float pixelThreshold = 0.1f;
};

struct ImageDiffResult {
    /// @brief 寸法が一致して比較できたか。false なら他の値は無意味。
    bool     comparable = false;
    /// @brief 全画素の RGB 差の平均 (0..1)。トーンの全体ずれを捉える。
    double   meanDiff = 0.0;
    /// @brief 違う画素の割合 (0..1)。小さな欠け (HUD が 1 個消えた) を捉える。
    double   badPixelRatio = 0.0;
    uint64_t badPixels = 0;
    /// @brief 基準画像を暗く敷き、違う画素を赤で塗った画像。
    RgbaImage diff;
};

/// @return 解読できなければ false。
[[nodiscard]] bool DecodePng(const std::vector<uint8_t>& png, RgbaImage& out);
/// @return 符号化に失敗したら false。
[[nodiscard]] bool EncodePng(const RgbaImage& image, std::vector<uint8_t>& out);

/// @brief アルファは比べない (RT の α はポスト処理で意味を持たないことが多い)。
[[nodiscard]] ImageDiffResult CompareImages(const RgbaImage& actual, const RgbaImage& baseline, const ImageDiffSettings& settings);

/// @return 読めなければ false。
[[nodiscard]] bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& out);
/// @brief 親ディレクトリを作ってから書く。
/// @return 書けなければ false。
[[nodiscard]] bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);

} // namespace fbzz::editor::playtest
