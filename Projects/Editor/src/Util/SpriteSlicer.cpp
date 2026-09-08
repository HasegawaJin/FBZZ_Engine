/// @file    SpriteSlicer.cpp
/// @brief   Sprite シートの矩形生成と既存矩形への畳み込みの実装。
/// @author  Hasegawa Jin
/// @date    2026-09-07
#include <Editor/Util/SpriteSlicer.hpp>
#include <Engine/Util/Uuid.hpp>
#include <stb_image.h>
#include <algorithm>
#include <queue>
#include <utility>

namespace fbzz::editor::spriteslice {

namespace {

constexpr std::uint8_t kAlphaThreshold = 8;

std::uint64_t OverlapArea(const asset::SpriteRect& lhs, const asset::SpriteRect& rhs)
{
    const std::uint64_t left   = std::max<std::uint64_t>(lhs.x, rhs.x);
    const std::uint64_t top    = std::max<std::uint64_t>(lhs.y, rhs.y);
    const std::uint64_t right  = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(lhs.x) + lhs.width,
        static_cast<std::uint64_t>(rhs.x) + rhs.width);
    const std::uint64_t bottom = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(lhs.y) + lhs.height,
        static_cast<std::uint64_t>(rhs.y) + rhs.height);
    return (right > left && bottom > top) ? (right - left) * (bottom - top) : 0ULL;
}

std::string MakeUniqueName(const std::vector<asset::SpriteRect>& sprites,
                           const std::string& requested)
{
    const auto taken = [&sprites](const std::string& name) {
        return std::any_of(sprites.begin(), sprites.end(),
            [&name](const asset::SpriteRect& sprite) { return sprite.name == name; });
    };
    if (!taken(requested)) return requested;
    for (int suffix = 1; ; ++suffix) {
        const std::string candidate = requested + "_" + std::to_string(suffix);
        if (!taken(candidate)) return candidate;
    }
}

} // namespace

Result GenerateGrid(const std::string& imagePath,
                    uint32_t textureWidth, uint32_t textureHeight,
                    const GridParams& params)
{
    Result result;
    if (textureWidth == 0 || textureHeight == 0) {
        result.error = "Texture dimensions are invalid.";
        return result;
    }

    const int offsetX  = std::clamp(params.offsetX, 0, static_cast<int>(textureWidth) - 1);
    const int offsetY  = std::clamp(params.offsetY, 0, static_cast<int>(textureHeight) - 1);
    const int paddingX = std::max(0, params.paddingX);
    const int paddingY = std::max(0, params.paddingY);

    int columns = 1;
    int rows = 1;
    int cellWidth = 1;
    int cellHeight = 1;
    if (params.byCellCount) {
        columns = std::clamp(params.columns, 1, static_cast<int>(textureWidth));
        rows    = std::clamp(params.rows,    1, static_cast<int>(textureHeight));
        const int usableWidth  = static_cast<int>(textureWidth)  - offsetX - paddingX * (columns - 1);
        const int usableHeight = static_cast<int>(textureHeight) - offsetY - paddingY * (rows - 1);
        if (usableWidth < columns || usableHeight < rows) {
            result.error = "Offset / Padding leaves no room for the requested grid.";
            return result;
        }
        cellWidth  = usableWidth / columns;
        cellHeight = usableHeight / rows;
    } else {
        cellWidth  = std::clamp(params.cellWidth,  1, static_cast<int>(textureWidth));
        cellHeight = std::clamp(params.cellHeight, 1, static_cast<int>(textureHeight));
        columns = std::max(1,
            (static_cast<int>(textureWidth)  - offsetX + paddingX) / (cellWidth  + paddingX));
        rows    = std::max(1,
            (static_cast<int>(textureHeight) - offsetY + paddingY) / (cellHeight + paddingY));
    }

    // 空セルを捨てるときだけ画素が要る。捨てないなら復号そのものを省く。
    int decodedWidth = 0;
    int decodedHeight = 0;
    int decodedChannels = 0;
    stbi_uc* decodedPixels = params.keepEmptyRects
        ? nullptr
        : stbi_load(imagePath.c_str(), &decodedWidth, &decodedHeight, &decodedChannels, 4);

    const auto hasVisiblePixels = [&](const asset::SpriteRect& rect) {
        if (decodedPixels == nullptr || decodedWidth <= 0 || decodedHeight <= 0) return true;
        const uint32_t maxX = std::min<uint32_t>(rect.x + rect.width,  static_cast<uint32_t>(decodedWidth));
        const uint32_t maxY = std::min<uint32_t>(rect.y + rect.height, static_cast<uint32_t>(decodedHeight));
        for (uint32_t y = rect.y; y < maxY; ++y) {
            for (uint32_t x = rect.x; x < maxX; ++x) {
                const size_t pixel = (static_cast<size_t>(y) * decodedWidth + x) * 4;
                if (decodedPixels[pixel + 3] > kAlphaThreshold) return true;
            }
        }
        return false;
    };

    const int countUsableWidth  = static_cast<int>(textureWidth)  - offsetX - paddingX * (columns - 1);
    const int countUsableHeight = static_cast<int>(textureHeight) - offsetY - paddingY * (rows - 1);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const int x = params.byCellCount
                ? offsetX + (countUsableWidth * column) / columns + paddingX * column
                : offsetX + column * (cellWidth + paddingX);
            const int y = params.byCellCount
                ? offsetY + (countUsableHeight * row) / rows + paddingY * row
                : offsetY + row * (cellHeight + paddingY);
            if (x >= static_cast<int>(textureWidth) || y >= static_cast<int>(textureHeight))
                continue;
            const int generatedWidth = params.byCellCount
                ? (countUsableWidth * (column + 1)) / columns - (countUsableWidth * column) / columns
                : cellWidth;
            const int generatedHeight = params.byCellCount
                ? (countUsableHeight * (row + 1)) / rows - (countUsableHeight * row) / rows
                : cellHeight;

            asset::SpriteRect sprite;
            sprite.id     = util::GenerateUUID();
            sprite.name   = params.baseName + std::to_string(result.sprites.size());
            sprite.x      = static_cast<uint32_t>(x);
            sprite.y      = static_cast<uint32_t>(y);
            sprite.width  = std::min<uint32_t>(static_cast<uint32_t>(generatedWidth),  textureWidth  - sprite.x);
            sprite.height = std::min<uint32_t>(static_cast<uint32_t>(generatedHeight), textureHeight - sprite.y);
            sprite.pivotX = std::clamp(params.pivotX, 0.0f, 1.0f);
            sprite.pivotY = std::clamp(params.pivotY, 0.0f, 1.0f);
            if (params.keepEmptyRects || hasVisiblePixels(sprite))
                result.sprites.push_back(std::move(sprite));
        }
    }
    if (decodedPixels) stbi_image_free(decodedPixels);

    if (result.sprites.empty())
        result.error = "Slice produced no rectangles.";
    return result;
}

Result GenerateAutoTrim(const std::string& imagePath, const AutoTrimParams& params)
{
    Result result;
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(imagePath.c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        result.error = "Automatic Slice could not decode this image.";
        return result;
    }

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> visited(pixelCount, 0);
    constexpr int DX[4] = { -1, 1, 0, 0 };
    constexpr int DY[4] = { 0, 0, -1, 1 };

    for (int seedY = 0; seedY < height; ++seedY) {
        for (int seedX = 0; seedX < width; ++seedX) {
            const size_t seed = static_cast<size_t>(seedY) * static_cast<size_t>(width)
                + static_cast<size_t>(seedX);
            if (visited[seed] || pixels[seed * 4 + 3] <= kAlphaThreshold) continue;

            int minX = seedX;
            int minY = seedY;
            int maxX = seedX;
            int maxY = seedY;
            std::queue<std::pair<int, int>> pending;
            pending.push({ seedX, seedY });
            visited[seed] = 1;
            while (!pending.empty()) {
                const auto [x, y] = pending.front();
                pending.pop();
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
                for (int direction = 0; direction < 4; ++direction) {
                    const int nx = x + DX[direction];
                    const int ny = y + DY[direction];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const size_t next = static_cast<size_t>(ny) * static_cast<size_t>(width)
                        + static_cast<size_t>(nx);
                    if (visited[next] || pixels[next * 4 + 3] <= kAlphaThreshold) continue;
                    visited[next] = 1;
                    pending.push({ nx, ny });
                }
            }

            asset::SpriteRect sprite;
            sprite.id     = util::GenerateUUID();
            sprite.name   = params.baseName + std::to_string(result.sprites.size());
            sprite.x      = static_cast<uint32_t>(minX);
            sprite.y      = static_cast<uint32_t>(minY);
            sprite.width  = static_cast<uint32_t>(maxX - minX + 1);
            sprite.height = static_cast<uint32_t>(maxY - minY + 1);
            sprite.pivotX = std::clamp(params.pivotX, 0.0f, 1.0f);
            sprite.pivotY = std::clamp(params.pivotY, 0.0f, 1.0f);
            result.sprites.push_back(std::move(sprite));
        }
    }
    stbi_image_free(pixels);

    if (result.sprites.empty())
        result.error = "Automatic Slice found no visible alpha islands.";
    return result;
}

std::vector<asset::SpriteRect> MergeIntoExisting(
    const std::vector<asset::SpriteRect>& existing,
    std::vector<asset::SpriteRect> generated,
    ExistingMode mode, int* outReusedCount)
{
    if (outReusedCount != nullptr) *outReusedCount = 0;
    if (mode == ExistingMode::DeleteExisting) return generated;

    std::vector<asset::SpriteRect> merged = existing;
    for (asset::SpriteRect& candidate : generated) {
        int bestIndex = -1;
        std::uint64_t bestArea = 0;
        for (int index = 0; index < static_cast<int>(merged.size()); ++index) {
            const std::uint64_t area = OverlapArea(candidate, merged[static_cast<size_t>(index)]);
            if (area > bestArea) {
                bestArea = area;
                bestIndex = index;
            }
        }
        if (bestIndex < 0) {
            candidate.name = MakeUniqueName(merged, candidate.name);
            merged.push_back(std::move(candidate));
            continue;
        }
        if (outReusedCount != nullptr) ++(*outReusedCount);
        // Smart は矩形だけ合わせ直す。ID はもちろん、名前・pivot・Border も残す。
        // WHY pivot まで残すか: どれも人が «この絵のための値» として詰めたもので、
        //     切り直しの意図は «位置がずれた» の修正でしかない。既定値で上書きすると
        //     直したはずが足元の当たりだけ静かにずれる。
        if (mode == ExistingMode::Smart) {
            asset::SpriteRect& target = merged[static_cast<size_t>(bestIndex)];
            target.x      = candidate.x;
            target.y      = candidate.y;
            target.width  = candidate.width;
            target.height = candidate.height;
        }
    }
    return merged;
}

} // namespace fbzz::editor::spriteslice
