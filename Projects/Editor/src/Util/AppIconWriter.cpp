/// @file    AppIconWriter.cpp
/// @brief   画像 → Windows アイコンリソース変換と exe への書き込み。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <Editor/Util/AppIconWriter.hpp>

#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <stb_image.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

/// 焼き込むサイズ。Explorer の特大アイコンから通知領域までを 1 グループで賄う。
constexpr std::array<int, 6> kAppIconSizes{ 256, 128, 64, 48, 32, 16 };

/// 1 グループに積む上限。ID は 1 から連番で振る。
constexpr int kMaxIconEntries = 32;

struct IconBitmap {
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> rgba; ///< 8bit RGBA、上の行から
};

/// exe へ書き込む 1 枚ぶん。bytes は DIB (自前変換) か .ico 内の画像そのまま。
struct IconEntry {
    int                       width    = 0;
    int                       height   = 0;
    std::uint16_t             bitCount = 32;
    std::vector<std::uint8_t> bytes;
};

#pragma pack(push, 1)
struct IconFileHeader {
    std::uint16_t reserved;
    std::uint16_t type;
    std::uint16_t count;
};
struct IconFileEntry {
    std::uint8_t  width;
    std::uint8_t  height;
    std::uint8_t  colorCount;
    std::uint8_t  reserved;
    std::uint16_t planes;
    std::uint16_t bitCount;
    std::uint32_t bytesInRes;
    std::uint32_t imageOffset;
};
/// RT_GROUP_ICON の 1 行。ファイル内オフセットの代わりに RT_ICON の ID を持つ。
struct IconGroupEntry {
    std::uint8_t  width;
    std::uint8_t  height;
    std::uint8_t  colorCount;
    std::uint8_t  reserved;
    std::uint16_t planes;
    std::uint16_t bitCount;
    std::uint32_t bytesInRes;
    std::uint16_t id;
};
#pragma pack(pop)

std::string IconPathUtf8(const std::filesystem::path& path)
{
    return util::FileSystem::PathToUtf8(path);
}

bool IsIcoPath(const std::filesystem::path& path)
{
    return util::StringUtils::ToLower(IconPathUtf8(path.extension())) == ".ico";
}

/// RT_ICON / RT_GROUP_ICON は UNICODE の定義次第で LPSTR にもなるマクロで、
/// そのままでは UpdateResourceW (LPCWSTR) に渡せない。W 版の ID として持ち直す。
LPCWSTR RtIcon()      { return MAKEINTRESOURCEW(3); }
LPCWSTR RtGroupIcon() { return MAKEINTRESOURCEW(14); }

std::string LastErrorText(const char* what)
{
    return std::string(what) + " (Win32 error " + std::to_string(GetLastError()) + ")";
}

/// 既にある RT_ICON の 1 枚。言語まで持つのは、削除が (ID, 言語) の組で効くため。
struct IconResourceId {
    WORD id;
    WORD language;
};

BOOL CALLBACK CollectIconLanguage(HMODULE, LPCWSTR, LPCWSTR name, WORD language, LONG_PTR param)
{
    if (IS_INTRESOURCE(name)) {
        reinterpret_cast<std::vector<IconResourceId>*>(param)->push_back(
            { static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name)), language });
    }
    return TRUE;
}

BOOL CALLBACK CollectIconName(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param)
{
    EnumResourceLanguagesW(module, type, name, CollectIconLanguage, param);
    return TRUE;
}

std::vector<IconResourceId> ExistingIcons(const std::filesystem::path& exePath)
{
    std::vector<IconResourceId> icons;
    HMODULE module = LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!module) return icons;
    EnumResourceNamesW(module, RtIcon(), CollectIconName, reinterpret_cast<LONG_PTR>(&icons));
    FreeLibrary(module);
    return icons;
}

/// 面積平均で縮小する。
/// @note アルファで重み付け: 透明ピクセルの RGB は多くの PNG で黒のまま残るため、単純平均だと縮小後の輪郭に黒い縁が出る。
IconBitmap ResizeIconBox(const IconBitmap& src, int dstW, int dstH)
{
    IconBitmap dst;
    dst.width  = dstW;
    dst.height = dstH;
    dst.rgba.assign(static_cast<std::size_t>(dstW) * dstH * 4, 0);

    for (int y = 0; y < dstH; ++y) {
        const int y0 = y * src.height / dstH;
        const int y1 = std::max(y0 + 1, (y + 1) * src.height / dstH);
        for (int x = 0; x < dstW; ++x) {
            const int x0 = x * src.width / dstW;
            const int x1 = std::max(x0 + 1, (x + 1) * src.width / dstW);

            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            int   samples = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* p =
                        &src.rgba[(static_cast<std::size_t>(sy) * src.width + sx) * 4];
                    const float alpha = p[3] / 255.0f;
                    r += p[0] * alpha;
                    g += p[1] * alpha;
                    b += p[2] * alpha;
                    a += alpha;
                    ++samples;
                }
            }

            std::uint8_t* d = &dst.rgba[(static_cast<std::size_t>(y) * dstW + x) * 4];
            const auto unpremultiply = [a](float sum) {
                return a > 0.0f
                    ? static_cast<std::uint8_t>(std::lround(std::clamp(sum / a, 0.0f, 255.0f)))
                    : std::uint8_t{ 0 };
            };
            d[0] = unpremultiply(r);
            d[1] = unpremultiply(g);
            d[2] = unpremultiply(b);
            d[3] = static_cast<std::uint8_t>(
                std::lround(std::clamp(a / static_cast<float>(samples) * 255.0f, 0.0f, 255.0f)));
        }
    }
    return dst;
}

IconBitmap ResizeIconBilinear(const IconBitmap& src, int dstW, int dstH)
{
    IconBitmap dst;
    dst.width  = dstW;
    dst.height = dstH;
    dst.rgba.assign(static_cast<std::size_t>(dstW) * dstH * 4, 0);

    const auto axis = [](int dstIndex, int dstSize, int srcSize, int& i0, int& i1, float& t) {
        const float center = (static_cast<float>(dstIndex) + 0.5f)
                           * static_cast<float>(srcSize) / static_cast<float>(dstSize) - 0.5f;
        const float clamped = std::clamp(center, 0.0f, static_cast<float>(srcSize - 1));
        i0 = static_cast<int>(clamped);
        i1 = std::min(i0 + 1, srcSize - 1);
        t  = clamped - static_cast<float>(i0);
    };

    for (int y = 0; y < dstH; ++y) {
        int y0 = 0, y1 = 0; float ty = 0.0f;
        axis(y, dstH, src.height, y0, y1, ty);
        for (int x = 0; x < dstW; ++x) {
            int x0 = 0, x1 = 0; float tx = 0.0f;
            axis(x, dstW, src.width, x0, x1, tx);

            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            const auto tap = [&](int sx, int sy, float weight) {
                const std::uint8_t* p =
                    &src.rgba[(static_cast<std::size_t>(sy) * src.width + sx) * 4];
                const float alpha = p[3] / 255.0f * weight;
                r += p[0] * alpha;
                g += p[1] * alpha;
                b += p[2] * alpha;
                a += alpha;
            };
            tap(x0, y0, (1.0f - tx) * (1.0f - ty));
            tap(x1, y0, tx * (1.0f - ty));
            tap(x0, y1, (1.0f - tx) * ty);
            tap(x1, y1, tx * ty);

            std::uint8_t* d = &dst.rgba[(static_cast<std::size_t>(y) * dstW + x) * 4];
            const auto unpremultiply = [a](float sum) {
                return a > 0.0f
                    ? static_cast<std::uint8_t>(std::lround(std::clamp(sum / a, 0.0f, 255.0f)))
                    : std::uint8_t{ 0 };
            };
            d[0] = unpremultiply(r);
            d[1] = unpremultiply(g);
            d[2] = unpremultiply(b);
            d[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a * 255.0f, 0.0f, 255.0f)));
        }
    }
    return dst;
}

/// side×side の正方形に収める。
/// @note 引き伸ばさない: アイコン枠は常に正方形で横長の絵を潰すと設定した絵と変わるため、縦横比を保ち中央に置き余白は透明にする。
IconBitmap FitIconToSquare(const IconBitmap& src, int side)
{
    const float scale = std::min(static_cast<float>(side) / static_cast<float>(src.width),
                                 static_cast<float>(side) / static_cast<float>(src.height));
    const int w = std::clamp(static_cast<int>(std::lround(src.width * scale)), 1, side);
    const int h = std::clamp(static_cast<int>(std::lround(src.height * scale)), 1, side);

    IconBitmap scaled = (w <= src.width && h <= src.height)
        ? ResizeIconBox(src, w, h)
        : ResizeIconBilinear(src, w, h);
    if (w == side && h == side) return scaled;

    IconBitmap out;
    out.width  = side;
    out.height = side;
    out.rgba.assign(static_cast<std::size_t>(side) * side * 4, 0);
    const int offsetX = (side - w) / 2;
    const int offsetY = (side - h) / 2;
    for (int y = 0; y < h; ++y) {
        std::memcpy(&out.rgba[(static_cast<std::size_t>(y + offsetY) * side + offsetX) * 4],
                    &scaled.rgba[static_cast<std::size_t>(y) * w * 4],
                    static_cast<std::size_t>(w) * 4);
    }
    return out;
}

/// アイコン 1 枚を 32bpp の DIB (BITMAPINFOHEADER + BGRA + AND マスク) にする。
/// @note PNG 圧縮にしない: PNG アイコンを読めるのは Vista 以降の一部の経路に限られエンコーダーも要るが、DIB ならどのサイズ・API でも同じに読める。
std::vector<std::uint8_t> EncodeIconDib(const IconBitmap& img)
{
    const std::size_t pixelBytes = static_cast<std::size_t>(img.width) * img.height * 4;
    const std::size_t maskStride = ((static_cast<std::size_t>(img.width) + 31) / 32) * 4;
    const std::size_t maskBytes  = maskStride * img.height;

    std::vector<std::uint8_t> out(sizeof(BITMAPINFOHEADER) + pixelBytes + maskBytes, 0);

    BITMAPINFOHEADER header{};
    header.biSize        = sizeof(BITMAPINFOHEADER);
    header.biWidth       = img.width;
    /// @note 2 倍にする: アイコンの DIB は「XOR (絵) + AND (マスク)」の 2 枚分の高さを書く約束。
    header.biHeight      = img.height * 2;
    header.biPlanes      = 1;
    header.biBitCount    = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage   = static_cast<DWORD>(pixelBytes + maskBytes);
    std::memcpy(out.data(), &header, sizeof(header));

    std::uint8_t* pixels = out.data() + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < img.height; ++y) {
        /// @note DIB の行は下から上。
        const std::uint8_t* srcRow =
            &img.rgba[static_cast<std::size_t>(img.height - 1 - y) * img.width * 4];
        std::uint8_t* dstRow = pixels + static_cast<std::size_t>(y) * img.width * 4;
        for (int x = 0; x < img.width; ++x) {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
        }
    }
    /// @note AND マスクは 0 のまま。32bpp では可視判定にアルファが使われる。
    return out;
}

bool DecodeIconImage(const std::vector<std::uint8_t>& fileBytes,
                     IconBitmap& out,
                     std::string& outError)
{
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()),
                                            &width, &height, &channels, 4);
    if (!pixels) {
        outError = "unsupported or corrupt image";
        return false;
    }
    out.width  = width;
    out.height = height;
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

/// .ico をそのまま配る経路。中の画像は再エンコードせず、ID を振り直して積み替えるだけ。
bool SplitIcoFile(const std::vector<std::uint8_t>& fileBytes,
                  std::vector<IconEntry>& out,
                  std::string& outError)
{
    if (fileBytes.size() < sizeof(IconFileHeader)) {
        outError = "not an .ico file";
        return false;
    }

    IconFileHeader header{};
    std::memcpy(&header, fileBytes.data(), sizeof(header));
    if (header.reserved != 0 || header.type != 1 || header.count == 0) {
        outError = "not an .ico file";
        return false;
    }

    for (std::uint16_t i = 0; i < header.count; ++i) {
        const std::size_t entryOffset = sizeof(IconFileHeader) + static_cast<std::size_t>(i) * sizeof(IconFileEntry);
        if (entryOffset + sizeof(IconFileEntry) > fileBytes.size()) {
            outError = "truncated .ico directory";
            return false;
        }

        IconFileEntry entry{};
        std::memcpy(&entry, fileBytes.data() + entryOffset, sizeof(entry));
        if (static_cast<std::size_t>(entry.imageOffset) + entry.bytesInRes > fileBytes.size()) {
            outError = "truncated .ico image data";
            return false;
        }

        IconEntry icon;
        icon.width    = entry.width == 0 ? 256 : entry.width;
        icon.height   = entry.height == 0 ? 256 : entry.height;
        icon.bitCount = entry.bitCount;
        icon.bytes.assign(fileBytes.begin() + entry.imageOffset,
                          fileBytes.begin() + entry.imageOffset + entry.bytesInRes);
        out.push_back(std::move(icon));
    }
    return !out.empty();
}

} // namespace


bool AppIconWriter::Inspect(const std::filesystem::path& imagePath,
                            SourceInfo& out,
                            std::string& outError)
{
    out = {};

    std::vector<std::uint8_t> fileBytes;
    if (!util::FileSystem::ReadBinary(imagePath, fileBytes) || fileBytes.empty()) {
        outError = "cannot read " + IconPathUtf8(imagePath);
        return false;
    }

    if (IsIcoPath(imagePath)) {
        std::vector<IconEntry> entries;
        if (!SplitIcoFile(fileBytes, entries, outError)) return false;
        out.isIco = true;
        for (const IconEntry& entry : entries) {
            out.width  = std::max(out.width, entry.width);
            out.height = std::max(out.height, entry.height);
        }
        return true;
    }

    int channels = 0;
    if (!stbi_info_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()),
                               &out.width, &out.height, &channels)) {
        outError = "unsupported or corrupt image";
        return false;
    }
    return true;
}

bool AppIconWriter::Apply(const std::filesystem::path& imagePath,
                          const std::filesystem::path& exePath,
                          std::string& outError)
{
    std::vector<std::uint8_t> fileBytes;
    if (!util::FileSystem::ReadBinary(imagePath, fileBytes) || fileBytes.empty()) {
        outError = "cannot read " + IconPathUtf8(imagePath);
        return false;
    }

    std::vector<IconEntry> entries;
    if (IsIcoPath(imagePath)) {
        if (!SplitIcoFile(fileBytes, entries, outError)) return false;
    } else {
        IconBitmap decoded;
        if (!DecodeIconImage(fileBytes, decoded, outError)) return false;

        for (int side : kAppIconSizes) {
            IconEntry entry;
            entry.width  = side;
            entry.height = side;
            entry.bytes  = EncodeIconDib(FitIconToSquare(decoded, side));
            entries.push_back(std::move(entry));
        }
    }

    if (entries.size() > static_cast<std::size_t>(kMaxIconEntries))
        entries.resize(static_cast<std::size_t>(kMaxIconEntries));

    /// @note 掃除する相手は更新を開ける前に数え上げる (LOAD_LIBRARY_AS_DATAFILE が
    ///       exe を掴んだままだと BeginUpdateResource が弾かれる)。
    const std::vector<IconResourceId> stale = ExistingIcons(exePath);

    /// @note 全消しにしない: 第 2 引数 TRUE は exe の既存リソースをすべて捨て DPI 対応マニフェストまで落ちるため、自分が書いたアイコンだけを消して書き直す。
    HANDLE update = BeginUpdateResourceW(exePath.c_str(), FALSE);
    if (!update) {
        outError = LastErrorText("BeginUpdateResource failed");
        return false;
    }

    const WORD language = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    const auto fail = [&update](std::string reason, std::string& sink) {
        /// @note 破棄
        EndUpdateResourceW(update, TRUE);
        sink = std::move(reason);
        return false;
    };

    /// @note 前回より多いサイズの置き去りを消す。持っている物だけ消す: 無い RT_ICON へ削除を出すと、
    ///       以降この更新ハンドルへの書き込みが全て ERROR_INTERNAL_ERROR (1359) を返す。アイコン無しの exe (典型的な対象) は 1 回目の空振りで全部落ちる。
    for (const IconResourceId& icon : stale) {
        if (!UpdateResourceW(update, RtIcon(), MAKEINTRESOURCEW(icon.id), icon.language,
                             nullptr, 0)) {
            return fail(LastErrorText("UpdateResource(RT_ICON delete) failed"), outError);
        }
    }

    std::vector<std::uint8_t> group(sizeof(IconFileHeader)
                                    + entries.size() * sizeof(IconGroupEntry));
    IconFileHeader groupHeader{ 0, 1, static_cast<std::uint16_t>(entries.size()) };
    std::memcpy(group.data(), &groupHeader, sizeof(groupHeader));

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const IconEntry& entry = entries[i];
        const WORD id = static_cast<WORD>(i + 1);

        if (!UpdateResourceW(update, RtIcon(), MAKEINTRESOURCEW(id), language,
                             const_cast<std::uint8_t*>(entry.bytes.data()),
                             static_cast<DWORD>(entry.bytes.size()))) {
            return fail(LastErrorText("UpdateResource(RT_ICON) failed"), outError);
        }

        IconGroupEntry row{};
        /// @note 256 は 0 で表す (1 バイトに収まらないため)。
        row.width      = static_cast<std::uint8_t>(entry.width  >= 256 ? 0 : entry.width);
        row.height     = static_cast<std::uint8_t>(entry.height >= 256 ? 0 : entry.height);
        row.colorCount = 0;
        row.reserved   = 0;
        row.planes     = 1;
        row.bitCount   = entry.bitCount;
        row.bytesInRes = static_cast<std::uint32_t>(entry.bytes.size());
        row.id         = id;
        std::memcpy(group.data() + sizeof(IconFileHeader) + i * sizeof(IconGroupEntry),
                    &row, sizeof(row));
    }

    if (!UpdateResourceW(update, RtGroupIcon(), MAKEINTRESOURCEW(kIconResourceId), language,
                         group.data(), static_cast<DWORD>(group.size()))) {
        return fail(LastErrorText("UpdateResource(RT_GROUP_ICON) failed"), outError);
    }

    if (!EndUpdateResourceW(update, FALSE)) {
        outError = LastErrorText("EndUpdateResource failed");
        return false;
    }
    return true;
}

} // namespace fbzz::editor
