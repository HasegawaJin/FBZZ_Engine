// FBZZ Engine
// FileUtil.cpp | fbzz::sandbox::util
// Sandbox のプロジェクト解決で使うファイル読み取りユーティリティ
#include "FileUtil.hpp"

#include <fstream>
#include <sstream>

namespace fbzz::sandbox::util {

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // namespace fbzz::sandbox::util
