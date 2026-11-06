/// @file    ShaderDependencyTracker.hpp
/// @brief   HLSLと再帰includeの更新時刻からCSOの鮮度を判定する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::renderer::shader_dependency {

/// @note 実行場所がbuild配下でもAssets相対パスを解決する。CWD探索とSDKのEngine assetルート探索は
/// @note ResolveShaderFilePath が一元管理しており、ここは呼び出し名を保つための委譲。
inline std::filesystem::path ResolveExistingPath(const std::filesystem::path& requested)
{
    return ResolveShaderFilePath(requested);
}

/// @note ソースの祖先からShadersディレクトリを特定し、ルートincludeの解決基準にする。
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

namespace detail {

enum class Condition { FALSE_VALUE, TRUE_VALUE, UNKNOWN };

inline Condition Negate(Condition value)
{
    return value == Condition::UNKNOWN ? value
        : value == Condition::TRUE_VALUE ? Condition::FALSE_VALUE : Condition::TRUE_VALUE;
}

inline Condition Both(Condition first, Condition second)
{
    if (first == Condition::FALSE_VALUE || second == Condition::FALSE_VALUE) return Condition::FALSE_VALUE;
    return first == Condition::TRUE_VALUE && second == Condition::TRUE_VALUE
        ? Condition::TRUE_VALUE : Condition::UNKNOWN;
}

inline std::string_view Trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

inline std::string_view TakeIdentifier(std::string_view& value)
{
    value = Trim(value);
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) return {};
    size_t length = 1;
    while (length < value.size()
        && (std::isalnum(static_cast<unsigned char>(value[length])) || value[length] == '_')) ++length;
    const auto name = value.substr(0, length);
    value.remove_prefix(length);
    return name;
}

struct Macro {
    std::vector<std::string> values;
    bool mayBeUndefined = false;
};

struct ScanState {
    std::unordered_map<std::string, Macro> macros;
    std::unordered_set<std::string> active;
    size_t revision = 0;
};

/// @note This dependency scan handles source-defined object macros, not arbitrary compiler preprocessor expressions.
/// @note Unknown conditional expressions scan both branches; unknown include expressions require recompilation.
inline bool ResolveInclude(std::string_view expression, const ScanState& state,
    std::unordered_set<std::string>& expanding, std::vector<std::filesystem::path>& names)
{
    expression = Trim(expression);
    if (expression.empty()) return false;
    if (expression.front() == '"' || expression.front() == '<') {
        const size_t end = expression.find(expression.front() == '<' ? '>' : '"', 1);
        if (end == std::string_view::npos || end == 1) return false;
        names.emplace_back(std::string(expression.substr(1, end - 1)));
        return true;
    }
    const std::string name(TakeIdentifier(expression));
    expression = Trim(expression);
    if (name.empty() || (!expression.empty() && !expression.starts_with("//"))) return false;
    const auto found = state.macros.find(name);
    if (found == state.macros.end() || !expanding.insert(name).second) return false;
    bool resolved = !found->second.values.empty();
    for (const auto& value : found->second.values)
        if (!ResolveInclude(value, state, expanding, names)) resolved = false;
    expanding.erase(name);
    return resolved;
}

inline bool HasNewerDependency(const std::filesystem::path& sourcePath,
    const std::filesystem::path& shaderRoot, const std::filesystem::file_time_type binaryTime,
    std::unordered_set<std::string>& visited, ScanState& state)
{
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(sourcePath, error);
    const std::string key = (error ? sourcePath.lexically_normal() : normalized).generic_string();
    if (state.active.contains(key)) return false;
    /// @note A second include may expand differently after a macro changes; only identical contexts are reusable.
    if (!visited.insert(key + "|" + std::to_string(state.revision)).second) return false;
    error.clear();
    const auto sourceTime = std::filesystem::last_write_time(sourcePath, error);
    if (error || sourceTime > binaryTime) return true;
    std::ifstream source(sourcePath);
    if (!source.is_open()) return true;
    state.active.insert(key);
    struct Conditional { Condition parent, previous; };
    std::vector<Conditional> conditions;
    Condition active = Condition::TRUE_VALUE;
    std::string line;
    while (std::getline(source, line)) {
        std::string_view text = Trim(line);
        if (text.empty() || text.front() != '#') continue;
        text.remove_prefix(1);
        const auto directive = TakeIdentifier(text);
        text = Trim(text);
        if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
            Condition condition = Condition::UNKNOWN;
            if (directive != "if") {
                const std::string name(TakeIdentifier(text));
                const auto found = state.macros.find(name);
                condition = found == state.macros.end() ? Condition::FALSE_VALUE
                    : found->second.mayBeUndefined ? Condition::UNKNOWN : Condition::TRUE_VALUE;
                if (directive == "ifndef") condition = Negate(condition);
            } else if (text == "0") condition = Condition::FALSE_VALUE;
            else if (text == "1") condition = Condition::TRUE_VALUE;
            conditions.push_back({active, condition});
            active = Both(active, condition);
            continue;
        }
        if (directive == "else" || directive == "elif") {
            if (conditions.empty()) return true;
            auto& current = conditions.back();
            const Condition condition = directive == "else" ? Condition::TRUE_VALUE
                : text == "0" ? Condition::FALSE_VALUE : text == "1" ? Condition::TRUE_VALUE : Condition::UNKNOWN;
            active = Both(current.parent, Both(Negate(current.previous), condition));
            current.previous = Negate(Both(Negate(current.previous), Negate(condition)));
            continue;
        }
        if (directive == "endif") {
            if (conditions.empty()) return true;
            active = conditions.back().parent;
            conditions.pop_back();
            continue;
        }
        /// @note Compiler-provided defines are not known here; literal includes retain the previous conservative dependency coverage.
        /// @note Inactive macro includes/definitions stay skipped so absent optional project hooks remain cacheable.
        if (active == Condition::FALSE_VALUE && (directive != "include" || text.empty()
            || (text.front() != '"' && text.front() != '<'))) continue;
        if (directive == "define" || directive == "undef") {
            const std::string name(TakeIdentifier(text));
            if (name.empty()) continue;
            auto found = state.macros.find(name);
            if (directive == "undef") {
                if (found == state.macros.end()) continue;
                if (active == Condition::TRUE_VALUE) { state.macros.erase(found); ++state.revision; }
                else if (!found->second.mayBeUndefined) { found->second.mayBeUndefined = true; ++state.revision; }
                continue;
            }
            const std::string replacement(Trim(text));
            if (active == Condition::TRUE_VALUE) {
                if (found == state.macros.end() || found->second.mayBeUndefined
                    || found->second.values != std::vector<std::string>{replacement}) {
                    state.macros[name] = {{replacement}, false};
                    ++state.revision;
                }
            } else {
                if (found == state.macros.end()) {
                    state.macros[name] = {{replacement}, true};
                    ++state.revision;
                } else if (std::find(found->second.values.begin(), found->second.values.end(), replacement)
                    == found->second.values.end()) {
                    found->second.values.push_back(replacement);
                    ++state.revision;
                }
            }
            continue;
        }
        if (directive != "include") continue;
        std::unordered_set<std::string> expanding;
        std::vector<std::filesystem::path> names;
        if (!ResolveInclude(text, state, expanding, names)) return true;
        for (const auto& includeName : names) {
            auto dependency = sourcePath.parent_path() / includeName;
            error.clear();
            if (!std::filesystem::is_regular_file(dependency, error)) dependency = shaderRoot / includeName;
            error.clear();
            if (!std::filesystem::is_regular_file(dependency, error))
                dependency = ResolveShaderFilePath(std::filesystem::path("Assets/Shaders") / includeName);
            error.clear();
            if (!std::filesystem::is_regular_file(dependency, error)
                || HasNewerDependency(dependency, shaderRoot, binaryTime, visited, state)) return true;
        }
    }
    state.active.erase(key);
    return !conditions.empty();
}

} /// @note namespace detail

/// @brief Checks literal and source-defined object-macro includes across one translation unit.
/// @note Undefined hooks guarded by #ifdef remain cacheable; unsupported include expressions conservatively rebuild.
inline bool HasNewerDependency(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& shaderRoot,
    const std::filesystem::file_time_type binaryTime,
    std::unordered_set<std::string>& visited)
{
    detail::ScanState state;
    return detail::HasNewerDependency(sourcePath, shaderRoot, binaryTime, visited, state);
}

/// @note HLSL本体または再帰includeがCSOより新しい場合、再コンパイルが必要と判定する。
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

} /// @note namespace fbzz::renderer::shader_dependency
