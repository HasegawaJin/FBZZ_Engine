/// @file    FluidSourceMask.cpp
/// @brief   テクスチャ発生源の濃さマスク (画像 → 256×256 の輝度 × α)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Engine/Asset/FluidSourceMask.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>

// WHY ここで展開するか: Engine の画像読み込みは Renderer 経由 (WIC / DirectXTex) で、焼きのワーカー
//     スレッドから COM を触らせたくない。STB_IMAGE_STATIC でこの翻訳単位に閉じ、Cursor.cpp や
//     Editor 側の stb 実装とは衝突させない。ファイルは自前で読む (非 ASCII のパスを fopen に渡さない)。
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>
// unity build で同じバッチの後続ファイルへ設定を漏らさない。
#undef STB_IMAGE_IMPLEMENTATION
#undef STB_IMAGE_STATIC
#undef STBI_ONLY_PNG
#undef STBI_ONLY_TGA
#undef STBI_ONLY_JPEG
#undef STBI_NO_STDIO
#undef STBI_NO_LINEAR
#undef STBI_NO_HDR

namespace fbzz::asset {
namespace {

struct StbiPixelsDeleter {
    void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};

std::filesystem::path MaskImageFile(const std::string& path)
{
    // guid: は AssetDatabase を直に引く。ResolveAssetPath の guid 分岐は «切れた参照» の報告済み集合を
    // ロックなしで書くので、焼きのワーカーから呼ぶと競合する (AssetDatabase の参照系はロックで守られている)。
    if (AssetDatabase::IsGuidRef(path))
        return util::FileSystem::PathFromUtf8(AssetDatabase::PathFromGuid(AssetDatabase::GuidFromRef(path)));
    const std::filesystem::path direct = util::FileSystem::PathFromUtf8(path);
    if (direct.is_absolute()) return direct;
    return util::FileSystem::PathFromUtf8(AssetManager::ResolveAssetPath(path));
}

// sRGB のまま重みを掛ける。マスクは «どれだけ湧くか» の目安で、見た目の明るさと揃っていれば足りる。
float MaskPixelValue(const stbi_uc* rgba)
{
    const float luminance = 0.2126f * static_cast<float>(rgba[0]) + 0.7152f * static_cast<float>(rgba[1])
                          + 0.0722f * static_cast<float>(rgba[2]);
    return luminance * static_cast<float>(rgba[3]) * (1.0f / (255.0f * 255.0f));
}

struct MaskTap {
    int index = 0;
    float weight = 0.0f;
};

/// 画像のどこを使うか (ピクセル)。既定は画像全体。
struct MaskCrop {
    int x = 0;
    int y = 0;
    /// 0 は «右端 / 下端まで» (SpriteRect の幅 0 = 画像全体という規約に合わせる)。
    int width = 0;
    int height = 0;
};

// Sprite 参照の切り抜きを .meta から引く。
// WHY ResolveSpriteReference を使わないか: あちらは UV を返すために元画像の寸法を先に要る。
//     ここは画像を自前で展開するので、必要なのはピクセル矩形だけ。
// WHY 見つからないときに画像全体へ落とさないか: 切れた参照が «アトラス全面» で湧くと、
//     板の形との区別が画面から付かない。読めなかったことにして呼び手に赤く言わせる。
bool SpriteCrop(const std::string& imageFile, const std::string& token, MaskCrop& out,
                std::string& outError)
{
    TextureImportSettings settings;
    if (!GetCachedTextureImportSettings(imageFile, settings)) {
        outError = "元画像の .meta を読めません (Texture Type は Sprite ですか): " + imageFile;
        return false;
    }
    if (const SpriteRect* sprite = FindSprite(settings, token)) {
        out = { static_cast<int>(sprite->x), static_cast<int>(sprite->y),
                static_cast<int>(sprite->width), static_cast<int>(sprite->height) };
        return true;
    }
    // sprites を 1 つも持たない Single は «全面 1 枚» を画像名で参照する (ResolveSpriteReference と同じ規約)。
    if (settings.sprites.empty() && settings.type == TextureType::Sprite
        && settings.spriteMode == SpriteMode::Single
        && token == util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(imageFile).stem())) {
        out = {};
        return true;
    }
    outError = "この ID / 名前の Sprite が .meta にありません: " + token;
    return false;
}

/// 元の sourceCount 画素を targetCount 画素へ写すときの、出力 1 画素ごとの重み (合計 1)。
std::vector<std::vector<MaskTap>> MaskResampleTaps(int sourceCount, int targetCount)
{
    std::vector<std::vector<MaskTap>> taps(static_cast<std::size_t>(targetCount));
    const float scale = static_cast<float>(sourceCount) / static_cast<float>(targetCount);
    for (int i = 0; i < targetCount; ++i) {
        std::vector<MaskTap>& row = taps[static_cast<std::size_t>(i)];
        if (scale > 1.0f) {
            // WHY 縮めるときは面積平均か: 双線形のまま大きく縮めると出力 1 画素が元の 2×2 画素しか拾わず、
            //     細い線 (文字・魔法陣) が途切れたり消えたりする。
            const float begin = static_cast<float>(i) * scale;
            const float end = begin + scale;
            const int first = static_cast<int>(std::floor(begin));
            const int last = (std::min)(static_cast<int>(std::ceil(end)), sourceCount);
            float total = 0.0f;
            for (int j = first; j < last; ++j) {
                const float overlap =
                    (std::min)(end, static_cast<float>(j + 1)) - (std::max)(begin, static_cast<float>(j));
                if (overlap <= 0.0f) continue;
                row.push_back({ j, overlap });
                total += overlap;
            }
            if (total > 0.0f)
                for (MaskTap& tap : row) tap.weight /= total;
        } else {
            const float center = (static_cast<float>(i) + 0.5f) * scale - 0.5f;
            const float base = std::floor(center);
            const float t = center - base;
            const int j = static_cast<int>(base);
            row.push_back({ std::clamp(j, 0, sourceCount - 1), 1.0f - t });
            row.push_back({ std::clamp(j + 1, 0, sourceCount - 1), t });
        }
    }
    return taps;
}

} // namespace

bool LoadFluidSourceMask(const std::string& path, FluidSourceMask& out, std::string* outError)
{
    out.values.clear();
    const auto fail = [outError](std::string message) {
        if (outError != nullptr) *outError = std::move(message);
        return false;
    };
    if (path.empty()) return fail("画像が指定されていません");

    std::string imagePath;
    std::string spriteToken;
    const bool isSprite = ParseSpriteReference(path, imagePath, spriteToken);

    const std::filesystem::path file = MaskImageFile(imagePath);
    std::vector<uint8_t> bytes;
    if (file.empty() || !util::FileSystem::ReadBinary(file, bytes) || bytes.empty())
        return fail("開けません: " + path);
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return fail("画像が大きすぎます: " + path);

    int width = 0;
    int height = 0;
    int channels = 0;
    const std::unique_ptr<stbi_uc, StbiPixelsDeleter> pixels(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4));
    if (!pixels || width <= 0 || height <= 0) {
        const char* reason = stbi_failure_reason();
        return fail("読めない画像です: " + path + " (" + (reason != nullptr ? reason : "unknown") + ")");
    }

    MaskCrop crop;
    if (isSprite) {
        std::string reason;
        if (!SpriteCrop(util::FileSystem::PathToUtf8(file), spriteToken, crop, reason))
            return fail(std::move(reason));
    }
    // 矩形が画像からはみ出していても中へ収める (Sprite Editor で切ったあとに元画像を差し替えられる)。
    const int cropX = std::clamp(crop.x, 0, width - 1);
    const int cropY = std::clamp(crop.y, 0, height - 1);
    const int cropWidth = crop.width > 0 ? (std::min)(crop.width, width - cropX) : width - cropX;
    const int cropHeight = crop.height > 0 ? (std::min)(crop.height, height - cropY) : height - cropY;

    const int size = kFluidSourceMaskSize;
    const std::vector<std::vector<MaskTap>> columnTaps = MaskResampleTaps(cropWidth, size);
    const std::vector<std::vector<MaskTap>> rowTaps = MaskResampleTaps(cropHeight, size);

    // 横だけ size へ写した中間 (切り抜きの行数 × size)。縦横を分けると 1 画素あたりの重みが «横 + 縦» 本で済む。
    std::vector<float> horizontal(static_cast<std::size_t>(cropHeight) * static_cast<std::size_t>(size), 0.0f);
    for (int y = 0; y < cropHeight; ++y) {
        const stbi_uc* sourceRow = pixels.get()
            + (static_cast<std::size_t>(cropY + y) * static_cast<std::size_t>(width)
               + static_cast<std::size_t>(cropX)) * 4;
        float* target = horizontal.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size);
        for (int x = 0; x < size; ++x) {
            float sum = 0.0f;
            for (const MaskTap& tap : columnTaps[static_cast<std::size_t>(x)])
                sum += tap.weight * MaskPixelValue(sourceRow + static_cast<std::size_t>(tap.index) * 4);
            target[x] = sum;
        }
    }

    std::vector<float> values(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), 0.0f);
    for (int y = 0; y < size; ++y) {
        const std::vector<MaskTap>& taps = rowTaps[static_cast<std::size_t>(y)];
        for (int x = 0; x < size; ++x) {
            float sum = 0.0f;
            for (const MaskTap& tap : taps)
                sum += tap.weight
                     * horizontal[static_cast<std::size_t>(tap.index) * static_cast<std::size_t>(size)
                                  + static_cast<std::size_t>(x)];
            values[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] =
                std::clamp(sum, 0.0f, 1.0f);
        }
    }
    out.values = std::move(values);
    return true;
}

float SampleFluidSourceMask(const FluidSourceMask& mask, float u, float v)
{
    if (!mask.IsValid()) return 1.0f;
    // NaN を int へ落とすと未定義動作になる。
    if (!std::isfinite(u)) u = 0.0f;
    if (!std::isfinite(v)) v = 0.0f;
    const int size = kFluidSourceMaskSize;
    // テクセル中心の規則 (GPU の双線形フィルター + clamp と同じ)。
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(size) - 0.5f;
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(size) - 0.5f;
    const float baseX = std::floor(x);
    const float baseY = std::floor(y);
    const float tx = x - baseX;
    const float ty = y - baseY;
    const int x0 = std::clamp(static_cast<int>(baseX), 0, size - 1);
    const int x1 = std::clamp(static_cast<int>(baseX) + 1, 0, size - 1);
    const int y0 = std::clamp(static_cast<int>(baseY), 0, size - 1);
    const int y1 = std::clamp(static_cast<int>(baseY) + 1, 0, size - 1);
    const auto at = [&mask, size](int px, int py) {
        return mask.values[static_cast<std::size_t>(py) * static_cast<std::size_t>(size) + static_cast<std::size_t>(px)];
    };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

} // namespace fbzz::asset
