/// @file    ShaderDependencyTracker.hpp
/// @brief   HLSLと再帰includeの更新時刻からCSOの鮮度を判定する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace fbzz::renderer::shader_dependency {

// 実行場所がbuild配下でもAssets相対パスを解決できるよう、親方向へ探索する。
inline std::filesystem::path ResolveExistingPath(const std::filesystem::path& requested)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(requested, error)) return requested;
    if (requested.is_absolute()) return requested;

    std::filesystem::path current = std::filesystem::current_path(error);
    if (error) return requested;
    for (;;) {
        const std::filesystem::path candidate = current / requested;
        error.clear();
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) break;
        current = parent;
    }
    return requested;
}

// ソースの祖先からShadersディレクトリを特定し、ルートincludeの解決基準にする。
inline std::filesystem::path FindShaderRoot(const std::filesystem::path& sourcePath)
{
    std::filesystem::path current = sourcePath.parent_path();
    while (!current.empty()) {
        std::string name = current.filename().string();
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == "shaders") return current;
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) break;
        current = parent;
    }
    return sourcePath.parent_path();
}

// 依存includeを再帰走査し、CSOより新しいソースが一つでもあればtrueを返す。
// WHY: HLSL本体だけの比較ではCommon/*.hlsli変更後も古いCSOを読み続けるため。
inline bool HasNewerDependency(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& shaderRoot,
    const std::filesystem::file_time_type binaryTime,
    std::unordered_set<std::string>& visited)
{
    std::error_code error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(sourcePath, error);
    const std::filesystem::path keyPath = error ? sourcePath.lexically_normal() : normalized;
    if (!visited.insert(keyPath.generic_string()).second) return false;

    error.clear();
    const auto sourceTime = std::filesystem::last_write_time(sourcePath, error);
    if (error || sourceTime > binaryTime) return true;

    std::ifstream source(sourcePath);
    if (!source.is_open()) return true;
    std::string line;
    while (std::getline(source, line)) {
        const size_t directive = line.find("#include");
        if (directive == std::string::npos) continue;
        const size_t begin = line.find_first_of("\"<", directive + 8);
        if (begin == std::string::npos) continue;
        const char close = line[begin] == '<' ? '>' : '\"';
        const size_t end = line.find(close, begin + 1);
        if (end == std::string::npos) continue;

        const std::filesystem::path includeName = line.substr(begin + 1, end - begin - 1);
        std::filesystem::path dependency = sourcePath.parent_path() / includeName;
        error.clear();
        if (!std::filesystem::is_regular_file(dependency, error))
            dependency = shaderRoot / includeName;
        error.clear();
        if (!std::filesystem::is_regular_file(dependency, error)) return true;
        if (HasNewerDependency(dependency, shaderRoot, binaryTime, visited)) return true;
    }
    return false;
}

// HLSL本体または再帰includeがCSOより新しい場合、再コンパイルが必要と判定する。
inline bool IsBinaryStale(const std::filesystem::path& source,
                          const std::filesystem::path& binary)
{
    const std::filesystem::path sourcePath = ResolveExistingPath(source);
    const std::filesystem::path binaryPath = ResolveExistingPath(binary);
    std::error_code error;
    if (!std::filesystem::is_regular_file(sourcePath, error)) return true;
    error.clear();
    if (!std::filesystem::is_regular_file(binaryPath, error)) return true;
    error.clear();
    const auto binaryTime = std::filesystem::last_write_time(binaryPath, error);
    if (error) return true;

    std::unordered_set<std::string> visited;
    return HasNewerDependency(sourcePath, FindShaderRoot(sourcePath), binaryTime, visited);
}

} // namespace fbzz::renderer::shader_dependency
