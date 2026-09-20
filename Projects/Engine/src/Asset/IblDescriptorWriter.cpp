/// @file    IblDescriptorWriter.cpp
/// @brief   IBL 記述子のディスク形式を Graphics から隔離する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Engine/Asset/IblDescriptorWriter.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
namespace fbzz::asset {
bool WriteIblDescriptor(const std::filesystem::path& path, const renderer::IblBakeOutput& output)
{
    FzIblHeader header{};
    std::memcpy(header.magic, "FZIBL\0", 6);
    header.version = static_cast<uint16_t>(FZIBL_VERSION);
    header.prefilteredMipCount = output.prefilteredMipCount;
    const auto copy = [&](char (&destination)[256], const std::string& source) {
        std::error_code error;
        auto relative = std::filesystem::relative(std::filesystem::path(source), path.parent_path(), error);
        if (error) relative = std::filesystem::path(source).filename();
        const auto text = relative.generic_string();
        if (text.size() >= sizeof(destination)) return false;
        std::memcpy(destination, text.c_str(), text.size() + 1);
        return true;
    };
    if (!copy(header.envCubemapPath, output.envCubemapPath) ||
        !copy(header.irradiancePath, output.irradiancePath) ||
        !copy(header.prefilteredPath, output.prefilteredPath) ||
        !copy(header.brdfLutPath, output.brdfLutPath)) {
        FBZZ_LOG_ERROR("IBL descriptor: relative path exceeds file format limit");
        return false;
    }
    std::ofstream stream(path, std::ios::binary);
    const auto bytes = std::bit_cast<std::array<char, sizeof(FzIblHeader)>>(header);
    stream.write(bytes.data(), bytes.size());
    if (stream) return true;
    FBZZ_LOG_ERROR("IBL descriptor: write failed [%s]", path.string().c_str());
    return false;
}
}
