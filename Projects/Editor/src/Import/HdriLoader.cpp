/// @file    HdriLoader.cpp
/// @brief   .hdr / .exr → float RGBA ピクセル読み込み。
/// @author  Hasegawa Jin
/// @date    2026-06-23

#include <stb_image.h>

/// @note miniz (zlib) は CMakeLists.txt で C ソースとして別コンパイルする。miniz.c の C tentative
///       definition を C++ 翻訳単位から #include すると MSVC C2086 (再定義) になるため。
#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>

#include <Editor/Import/HdriLoader.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace fbzz::editor {

namespace {

std::string LowerExt(const std::string& absPath)
{
    std::string ext = fbzz::util::FileSystem::GetExtension(absPath);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/// RGB float → RGBA float 変換 (TinyEXR は RGB で返すことがある)
std::unique_ptr<float[]> RGBToRGBA(const float* rgb, uint32_t w, uint32_t h)
{
    const uint32_t n = w * h;
    auto rgba = std::make_unique<float[]>(n * 4u);
    for (uint32_t i = 0; i < n; ++i)
    {
        rgba[i * 4u + 0] = rgb[i * 3u + 0];
        rgba[i * 4u + 1] = rgb[i * 3u + 1];
        rgba[i * 4u + 2] = rgb[i * 3u + 2];
        rgba[i * 4u + 3] = 1.0f;
    }
    return rgba;
}

} // namespace

HdriPixels HdriLoader::Load(const std::string& absPath)
{
    const std::string ext = LowerExt(absPath);

    /// @name .hdr (Radiance RGBE)
    if (ext == ".hdr")
    {
        int w = 0, h = 0, ch = 0;
        /// @note stbi_loadf は RGBE を float RGBA にデコードする。4ch を要求する。
        float* raw = stbi_loadf(absPath.c_str(), &w, &h, &ch, 4);
        if (!raw) {
            FBZZ_LOG_ERROR("HdriLoader: .hdr 読み込み失敗 [%s] %s",
                           absPath.c_str(), stbi_failure_reason());
            return {};
        }

        HdriPixels result;
        result.data  = { raw, [](void* p){ stbi_image_free(p); } };
        result.width  = static_cast<uint32_t>(w);
        result.height = static_cast<uint32_t>(h);
        return result;
    }

    /// @name .exr (OpenEXR)
    if (ext == ".exr")
    {
        float*      raw  = nullptr;
        int         w    = 0, h = 0;
        const char* err  = nullptr;

        /// @note LoadEXR は RGBA float* を malloc で確保して返す。失敗時は err にメッセージを設定。
        const int ret = LoadEXR(&raw, &w, &h, absPath.c_str(), &err);
        if (ret != TINYEXR_SUCCESS) {
            FBZZ_LOG_ERROR("HdriLoader: .exr 読み込み失敗 [%s] %s",
                           absPath.c_str(), err ? err : "(不明なエラー)");
            /// @note TinyEXR のエラーメッセージを解放
            FreeEXRErrorMessage(err);
            return {};
        }

        /// @note TinyEXR v1.x は RGBA float を返す (4ch)
        HdriPixels result;
        result.data  = { raw, [](void* p){ free(p); } };
        result.width  = static_cast<uint32_t>(w);
        result.height = static_cast<uint32_t>(h);
        return result;
    }

    FBZZ_LOG_WARN("HdriLoader: 非対応の拡張子 [%s]", absPath.c_str());
    return {};
}

} // namespace fbzz::editor
