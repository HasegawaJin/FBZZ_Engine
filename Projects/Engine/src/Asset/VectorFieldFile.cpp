/// @file    VectorFieldFile.cpp
/// @brief   速度場 PNG / .fga の読み書き。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Engine/Asset/VectorFieldFile.hpp>
#include "FlipbookImageIO.hpp"
#include <Engine/Util/FileSystem.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DirectXTex.h>
#include <toml++/toml.hpp>

#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

namespace fbzz::asset {
namespace {

constexpr uint32_t kMaxResolution = fluid::kMaxVectorFieldResolution;

/// @brief 呼び出しスレッドで COM を使える状態にする。
/// @return COM が使えるか。既に別モード (STA) で初期化済みの RPC_E_CHANGED_MODE も使える扱い。
/// @note CoUninitialize は呼ばない。DirectXTex は WIC ファクトリをプロセス全体でキャッシュするため、
///       最後の参照で COM を畳むと次回の呼び出しが解放済みのファクトリを掴む。同じプロセスで
///       保存と読み込みを続けると 2 回目以降が必ず失敗する。
/// @see https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex (CoInitializeEx, Return value)
bool EnsureCom()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

bool HasPngExtension(const std::string& path)
{
    std::string ext = util::FileSystem::PathFromUtf8(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".png";
}

bool ReadFieldMeta(const std::string& path, toml::table& root)
{
    std::string text;
    if (!util::FileSystem::ReadText(path + ".meta", text)) return false;
    const auto parsed = toml::parse(text);
    if (!parsed) return false;
    root = parsed.table();
    return true;
}

bool SaveVectorFieldPng(const std::string& path, const fluid::VectorFieldAsset& source)
{
    if (source.Empty() || source.sizeX > kMaxResolution || source.sizeY > kMaxResolution
        || source.sizeZ > kMaxResolution
        || source.data.size() != static_cast<size_t>(source.sizeX) * source.sizeY * source.sizeZ)
        return false;
    const auto finite = [](const math::Vector3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite(source.boundsMin) || !finite(source.boundsMax)
        || source.boundsMin.x >= source.boundsMax.x || source.boundsMin.y >= source.boundsMax.y
        || source.boundsMin.z >= source.boundsMax.z) return false;
    for (const auto& value : source.data) if (!finite(value)) return false;
    /// @note 32³へリサンプルしてから量子化する。CPU と GPU の格子を揃える。
    auto field = source;
    fluid::NormalizeVectorField(field);
    std::vector<uint8_t> pixels(field.data.size() * 4, 255);
    for (size_t i = 0; i < field.data.size(); ++i) {
        pixels[i * 4] = fluid::EncodeVectorFieldByte(field.data[i].x, field.maxMagnitude);
        pixels[i * 4 + 1] = fluid::EncodeVectorFieldByte(field.data[i].y, field.maxMagnitude);
        pixels[i * 4 + 2] = fluid::EncodeVectorFieldByte(field.data[i].z, field.maxMagnitude);
    }
    toml::table root;
    if (util::FileSystem::Exists(path + ".meta") && !ReadFieldMeta(path, root)) return false;
    toml::table descriptor;
    descriptor.insert("version", 1);
    descriptor.insert("size", toml::array{ field.sizeX, field.sizeY, field.sizeZ });
    descriptor.insert("bounds_min", toml::array{ field.boundsMin.x, field.boundsMin.y, field.boundsMin.z });
    descriptor.insert("bounds_max", toml::array{ field.boundsMax.x, field.boundsMax.y, field.boundsMax.z });
    descriptor.insert("max_magnitude", field.maxMagnitude);
    root.insert_or_assign("vector_field", std::move(descriptor));
    /// @note 通常の画像として開いても速度値を色変換・圧縮・縮小しない。
    root.insert_or_assign("texture", toml::table{
        { "type", "data" }, { "srgb", false }, { "compression", "None" },
        { "mipmaps", false }, { "max_size", 16384 } });
    if (!EnsureCom()) return false;
    std::string error;
    if (!detail::SavePngRgba8(util::FileSystem::PathFromUtf8(path), field.sizeX,
                             field.sizeY * field.sizeZ, pixels, error)) {
        FBZZ_LOG_ERROR("VectorField: %s", error.c_str());
        return false;
    }
    std::ostringstream text;
    text << root;
    return util::FileSystem::WriteTextAtomic(path + ".meta", text.str());
}

bool LoadVectorFieldPng(const std::string& path, fluid::VectorFieldAsset& outField)
{
    toml::table root;
    if (!ReadFieldMeta(path, root) || root["vector_field"]["version"].value_or(0) != 1) return false;
    const auto descriptor = root["vector_field"];
    const auto* sizes = descriptor["size"].as_array();
    const auto* minBounds = descriptor["bounds_min"].as_array();
    const auto* maxBounds = descriptor["bounds_max"].as_array();
    if (!sizes || sizes->size() != 3 || !minBounds || minBounds->size() != 3
        || !maxBounds || maxBounds->size() != 3) return false;
    fluid::VectorFieldAsset field;
    field.sizeX = (*sizes)[0].value_or(0u);
    field.sizeY = (*sizes)[1].value_or(0u);
    field.sizeZ = (*sizes)[2].value_or(0u);
    if (field.sizeX == 0 || field.sizeY == 0 || field.sizeZ == 0
        || field.sizeX > kMaxResolution || field.sizeY > kMaxResolution || field.sizeZ > kMaxResolution)
        return false;
    field.maxMagnitude = descriptor["max_magnitude"].value_or(0.0f);
    if (!std::isfinite(field.maxMagnitude) || field.maxMagnitude <= 0.0f) return false;
    float lower[3], upper[3];
    for (size_t i = 0; i < 3; ++i) {
        const auto a = (*minBounds)[i].value<float>();
        const auto b = (*maxBounds)[i].value<float>();
        if (!a || !b || !std::isfinite(*a) || !std::isfinite(*b) || *a >= *b) return false;
        lower[i] = *a;
        upper[i] = *b;
    }
    field.boundsMin = { lower[0], lower[1], lower[2] };
    field.boundsMax = { upper[0], upper[1], upper[2] };
    if (!EnsureCom()) return false;
    DirectX::ScratchImage image;
    const auto imagePath = util::FileSystem::PathFromUtf8(path);
    if (FAILED(DirectX::LoadFromWICFile(imagePath.c_str(),
        DirectX::WIC_FLAGS_FORCE_RGB | DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, image))) return false;
    const DirectX::Image* source = image.GetImage(0, 0, 0);
    if (!source || source->width != field.sizeX || source->height != field.sizeY * field.sizeZ) return false;
    DirectX::ScratchImage converted;
    if (source->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (FAILED(DirectX::Convert(*source, DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, 0.0f, converted))) return false;
        source = converted.GetImage(0, 0, 0);
    }
    field.data.resize(static_cast<size_t>(field.sizeX) * field.sizeY * field.sizeZ);
    const auto decode = [scale = field.maxMagnitude](uint8_t byte) {
        return (static_cast<float>(byte) / 255.0f * 2.0f - 1.0f) * scale;
    };
    for (size_t y = 0; y < source->height; ++y) {
        const auto* row = source->pixels + y * source->rowPitch;
        for (size_t x = 0; x < source->width; ++x)
            field.data[y * field.sizeX + x] = { decode(row[x * 4]), decode(row[x * 4 + 1]), decode(row[x * 4 + 2]) };
    }
    outField = std::move(field);
    return true;
}

} // namespace

bool SaveVectorField(const std::string& absPath, const fluid::VectorFieldAsset& field)
{
    return HasPngExtension(absPath) && SaveVectorFieldPng(absPath, field);
}

bool LoadVectorFieldFile(const std::string& absPath, fluid::VectorFieldAsset& outField)
{
    return HasPngExtension(absPath) && LoadVectorFieldPng(absPath, outField);
}

bool IsVectorFieldPng(const std::string& absPath)
{
    toml::table root;
    return HasPngExtension(absPath) && ReadFieldMeta(absPath, root)
        && root["vector_field"]["version"].value_or(0) == 1;
}

/// @note .fga は 1 行目にヘッダー、以降カンマ区切りで xyz が並ぶ ASCII
///       (sizeX,sizeY,sizeZ, minX,minY,minZ, maxX,maxY,maxZ, x,y,z, ...)。
/// @note 改行の位置は書き出したツールによって違うので、区切りだけを見て数値を順に取る。
bool ImportFgaFile(const std::string& absPath, fluid::VectorFieldAsset& outField)
{
    std::ifstream in(absPath);
    if (!in) return false;

    std::vector<float> values;
    std::string token;
    while (std::getline(in, token, ',')) {
        /// @note 行末の改行が混ざるので、数値として読めた分だけを拾う。
        std::istringstream parse(token);
        float value = 0.0f;
        if (parse >> value) values.push_back(value);
    }
    if (values.size() < 9) {
        FBZZ_LOG_ERROR("VectorField: .fga のヘッダーが読めません: %s", absPath.c_str());
        return false;
    }

    fluid::VectorFieldAsset field;
    field.sizeX = static_cast<uint32_t>((std::max)(values[0], 0.0f));
    field.sizeY = static_cast<uint32_t>((std::max)(values[1], 0.0f));
    field.sizeZ = static_cast<uint32_t>((std::max)(values[2], 0.0f));
    if (field.sizeX == 0 || field.sizeY == 0 || field.sizeZ == 0
        || field.sizeX > kMaxResolution || field.sizeY > kMaxResolution
        || field.sizeZ > kMaxResolution) {
        FBZZ_LOG_ERROR("VectorField: .fga の解像度が範囲外 (%ux%ux%u): %s",
                       field.sizeX, field.sizeY, field.sizeZ, absPath.c_str());
        return false;
    }
    /// @note .fga は cm 基準で書かれる。エンジンは m なので 1/100 する。
    constexpr float kCmToM = 0.01f;
    field.boundsMin = { values[3] * kCmToM, values[4] * kCmToM, values[5] * kCmToM };
    field.boundsMax = { values[6] * kCmToM, values[7] * kCmToM, values[8] * kCmToM };

    const size_t count = static_cast<size_t>(field.sizeX) * field.sizeY * field.sizeZ;
    if (values.size() < 9 + count * 3) {
        FBZZ_LOG_ERROR("VectorField: .fga のベクトル数が足りません (%zu / %zu): %s",
                       (values.size() - 9) / 3, count, absPath.c_str());
        return false;
    }
    field.data.resize(count);
    for (size_t i = 0; i < count; ++i) {
        field.data[i] = { values[9 + i * 3 + 0], values[9 + i * 3 + 1], values[9 + i * 3 + 2] };
    }
    outField = std::move(field);
    return true;
}


} // namespace fbzz::asset
