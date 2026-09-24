/// @file    TextureFileDecoder.cpp
/// @brief   DirectXTex で読んだ画像を RGBA8 ミップ列へ展開する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>

#include <Graphics/Renderer/TextureFileDecoder.hpp>
#include <Core/Util/StringUtils.hpp>

#include <DirectXTex.h>

#include <algorithm>
#include <cstring>

/// @note DirectXTex/WIC が内部で CoCreateInstance を呼ぶため ole32.lib をリンクする。
#pragma comment(lib, "ole32.lib")

namespace fbzz::renderer {

namespace {

/// @brief 呼び出しの間だけ COM を MTA で初期化する。既に初期化済みのスレッドでは何もしない。
/// @note RPC_E_CHANGED_MODE は «既に STA で初期化済み» で、WIC はそのまま使えるので失敗扱いにしない。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex CoInitializeEx «Return value»
class ScopedComApartment {
public:
    ScopedComApartment()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ownsApartment = SUCCEEDED(result);
    }
    ~ScopedComApartment()
    {
        if (m_ownsApartment) CoUninitialize();
    }
    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;

private:
    bool m_ownsApartment = false;
};

bool EndsWithCI(const std::string& text, const char* suffix)
{
    const std::size_t length = std::strlen(suffix);
    if (text.size() < length) return false;
    return _stricmp(text.c_str() + text.size() - length, suffix) == 0;
}

void SetError(std::string* outError, const char* message, const std::string& path)
{
    if (outError) *outError = std::string(message) + ": " + path;
}

} /// @note namespace

std::size_t DecodedTextureRGBA8::ByteSize() const
{
    std::size_t bytes = 0;
    for (const Mip& mip : mips) bytes += mip.rgba.size();
    return bytes;
}

bool DecodedTextureRGBA8::ToMipData(std::vector<TextureMipData>& out) const
{
    out.clear();
    if (mips.empty()) return false;
    out.reserve(mips.size());
    for (const Mip& mip : mips) {
        if (mip.width == 0 || mip.height == 0 || mip.rgba.empty()) return false;
        out.push_back({ mip.rgba.data(), mip.width, mip.height });
    }
    return true;
}

bool DecodeTextureFileRGBA8(const std::string& path, DecodedTextureRGBA8& out, std::string* outError,
                            std::uint32_t dropTopLevels)
{
    out.mips.clear();
    out.sourceWidth = 0;
    out.sourceHeight = 0;
    const ScopedComApartment apartment;

    DirectX::ScratchImage source;
    const std::wstring widePath = util::StringUtils::ToWide(path);
    HRESULT result = E_FAIL;
    if (EndsWithCI(path, ".dds"))
        result = DirectX::LoadFromDDSFile(widePath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, source);
    else if (EndsWithCI(path, ".tga"))
        result = DirectX::LoadFromTGAFile(widePath.c_str(), nullptr, source);
    else
        result = DirectX::LoadFromWICFile(widePath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, source);
    if (FAILED(result) || source.GetImageCount() == 0) {
        SetError(outError, "画像を読み込めません", path);
        return false;
    }

    out.sourceWidth  = static_cast<std::uint32_t>(source.GetMetadata().width);
    out.sourceHeight = static_cast<std::uint32_t>(source.GetMetadata().height);

    /// @note DDS に入っているミップは全段展開する。フリップブックの «コマを跨がないミップ» は
    /// @note       焼く側でしか作れないので、ここで 0 段目だけにすると遠くの粒子がちらつく。
    const std::size_t mipCount = (std::max)(source.GetMetadata().mipLevels, std::size_t{ 1 });
    /// @note 品質段: ミップを持つ画像は上位の段を捨てるだけで済む。1 段だけの画像は展開後に縮小する。
    const std::size_t firstLevel = mipCount > 1
        ? (std::min)(static_cast<std::size_t>(dropTopLevels), mipCount - 1) : 0;

    out.mips.reserve(mipCount - firstLevel);
    for (std::size_t level = firstLevel; level < mipCount; ++level) {
        const DirectX::Image* image = source.GetImage(level, 0, 0);
        if (image == nullptr) break;

        DirectX::ScratchImage converted;
        if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            result = DirectX::IsCompressed(image->format)
                ? DirectX::Decompress(*image, DXGI_FORMAT_R8G8B8A8_UNORM, converted)
                : DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   DirectX::TEX_FILTER_DEFAULT, 0.0f, converted);
            if (FAILED(result)) {
                SetError(outError, "RGBA8 変換に失敗しました", path);
                out.mips.clear();
                return false;
            }
            image = converted.GetImage(0, 0, 0);
        }

        DecodedTextureRGBA8::Mip& mip = out.mips.emplace_back();
        mip.width  = static_cast<std::uint32_t>(image->width);
        mip.height = static_cast<std::uint32_t>(image->height);
        const std::size_t rowBytes = static_cast<std::size_t>(mip.width) * 4u;
        mip.rgba.resize(rowBytes * mip.height);
        for (std::uint32_t row = 0; row < mip.height; ++row)
            std::memcpy(mip.rgba.data() + row * rowBytes, image->pixels + row * image->rowPitch, rowBytes);
    }
    if (out.mips.empty()) {
        SetError(outError, "画像に 0 段目がありません", path);
        return false;
    }
    if (mipCount == 1)
        for (std::uint32_t i = 0; i < dropTopLevels && (out.mips[0].width > 1 || out.mips[0].height > 1); ++i)
            out.mips[0] = DownsampleRGBA8Box2x(out.mips[0]);
    return true;
}

void FlipTextureGreen(DecodedTextureRGBA8& texture)
{
    for (DecodedTextureRGBA8::Mip& mip : texture.mips) {
        for (std::size_t pixel = 1; pixel < mip.rgba.size(); pixel += 4)
            mip.rgba[pixel] = static_cast<std::uint8_t>(255u - mip.rgba[pixel]);
    }
}

DecodedTextureRGBA8::Mip DownsampleRGBA8Box2x(const DecodedTextureRGBA8::Mip& source)
{
    DecodedTextureRGBA8::Mip result;
    result.width  = (std::max)(source.width / 2u, 1u);
    result.height = (std::max)(source.height / 2u, 1u);
    result.rgba.resize(static_cast<std::size_t>(result.width) * result.height * 4u);
    const auto texel = [&](std::uint32_t x, std::uint32_t y, std::uint32_t channel) -> std::uint32_t {
        x = (std::min)(x, source.width - 1u);
        y = (std::min)(y, source.height - 1u);
        return source.rgba[(static_cast<std::size_t>(y) * source.width + x) * 4u + channel];
    };
    for (std::uint32_t y = 0; y < result.height; ++y) {
        for (std::uint32_t x = 0; x < result.width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                /// @note 2x2 の平均を四捨五入する。奇数幅の端は最後の列を重ねて使う。
                const std::uint32_t sum = texel(x * 2, y * 2, c) + texel(x * 2 + 1, y * 2, c)
                                        + texel(x * 2, y * 2 + 1, c) + texel(x * 2 + 1, y * 2 + 1, c);
                result.rgba[(static_cast<std::size_t>(y) * result.width + x) * 4u + c] =
                    static_cast<std::uint8_t>((sum + 2u) / 4u);
            }
        }
    }
    return result;
}

} /// @note namespace fbzz::renderer
