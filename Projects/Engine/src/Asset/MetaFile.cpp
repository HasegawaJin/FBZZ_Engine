/// @file    MetaFile.cpp
/// @brief   アセットメタデータのLF保存と同値な再保存の抑止。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#include <Engine/Asset/MetaFile.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <filesystem>
#include <string_view>

namespace fbzz::asset {
namespace {

std::string NormalizeLineEndings(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') continue;
        result.push_back(text[index]);
    }
    return result;
}

} /// @note namespace

bool MetaFile::Save(const std::string& path, const std::string& text)
{
    std::string output = NormalizeLineEndings(text);
    const auto desired = toml::parse(output);
    if (!desired) {
        FBZZ_LOG_ERROR("MetaFile: invalid TOML output [%s]", path.c_str());
        return false;
    }

    const auto nativePath = util::FileSystem::PathFromUtf8(path);
    std::error_code error;
    const bool exists = std::filesystem::exists(nativePath, error);
    if (error) return false;
    if (exists) {
        std::string existing;
        if (!util::FileSystem::ReadText(nativePath, existing)) return false;
        const auto parsed = toml::parse(existing);
        if (!parsed) {
            FBZZ_LOG_ERROR("MetaFile: refusing to overwrite invalid TOML [%s]", path.c_str());
            return false;
        }
        if (parsed.table() == desired.table()) output = NormalizeLineEndings(existing);
        if (existing == output) return true;
    }
    return util::FileSystem::WriteTextAtomic(path, output);
}

} /// @note namespace fbzz::asset
