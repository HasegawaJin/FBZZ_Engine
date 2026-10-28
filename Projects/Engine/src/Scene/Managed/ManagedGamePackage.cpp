/// @file    ManagedGamePackage.cpp
/// @brief   C# の配布起動と適用済み開発成果物の解決。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Engine/Scene/Managed/ManagedGamePackage.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <toml++/toml.hpp>
#include <algorithm>

namespace fbzz::scene {
namespace {
bool Fail(std::string& diagnostic, const std::string& message)
{
    diagnostic = message;
    return false;
}
bool File(const std::filesystem::path& path, std::string& diagnostic)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) return true;
    return Fail(diagnostic, "Managed file is missing or unreadable: " + util::FileSystem::PathToUtf8(path));
}
bool Leaf(const std::string& value)
{
    if (value.empty() || value == "." || value == ".." || value.find('\0') != std::string::npos) return false;
    const std::filesystem::path path = util::FileSystem::PathFromUtf8(value);
    return !path.has_parent_path() && !path.has_root_path() && value.find(':') == std::string::npos;
}
} /// @note namespace

bool FindManagedGameProject(const std::filesystem::path& root, std::filesystem::path& project, std::string& diagnostic)
{
    std::filesystem::path found;
    std::error_code error;
    std::filesystem::directory_iterator iterator(root, error);
    if (error) return Fail(diagnostic, "Managed project directory is unreadable: " + util::FileSystem::PathToUtf8(root));
    for (; iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        if (error) return Fail(diagnostic, "Managed project enumeration failed");
        const std::filesystem::path path = iterator->path();
        if (util::StringUtils::ToLower(path.extension().string()) != ".csproj" || !path.stem().wstring().ends_with(L".Scripts")) continue;
        if (!found.empty()) return Fail(diagnostic, "Expected one <Project>.Scripts.csproj");
        found = path;
    }
    if (error) return Fail(diagnostic, "Managed project enumeration failed");
    project = std::move(found);
    diagnostic.clear();
    return true;
}

bool ValidateManagedGameFiles(const std::filesystem::path& assembly, std::string& diagnostic)
{
    if (util::StringUtils::ToLower(assembly.extension().string()) != ".dll") return Fail(diagnostic, "Managed assembly must be a DLL");
    std::filesystem::path dependencies = assembly;
    dependencies.replace_extension(L".deps.json");
    if (!File(assembly, diagnostic) || !File(dependencies, diagnostic) || !File(assembly.wstring() + L".scripts.json", diagnostic)) return false;
    diagnostic.clear();
    return true;
}

bool ResolveManagedGameAssembly(const std::filesystem::path& root, std::string_view configuration,
    std::filesystem::path& assembly, std::string& diagnostic)
{
    std::filesystem::path resolved;
    const std::filesystem::path manifest = root / L"Managed" / L"game.managed.toml";
    std::error_code error;
    const bool exists = std::filesystem::exists(manifest, error);
    if (error) return Fail(diagnostic, "Managed package manifest is unreadable");
    if (exists) {
        std::string text;
        if (!util::FileSystem::ReadText(manifest, text)) return Fail(diagnostic, "Managed package manifest could not be read");
        auto parsed = toml::parse(text);
        if (!parsed || parsed.table()["schema_version"].value_or(0) != 1) return Fail(diagnostic, "Unsupported managed package manifest");
        const std::string name = parsed.table()["assembly"].value_or(std::string{});
        if (!Leaf(name)) return Fail(diagnostic, "Managed package assembly must be a filename inside Managed");
        resolved = manifest.parent_path() / util::FileSystem::PathFromUtf8(name);
    } else {
        std::filesystem::path project;
        if (!FindManagedGameProject(root, project, diagnostic)) return false;
        if (project.empty()) { assembly.clear(); diagnostic.clear(); return true; }
        const std::filesystem::path output = root / L"Library" / L"ManagedScripts" / std::string(configuration);
        std::string generation;
        if (!util::FileSystem::ReadText(output / L"current.txt", generation)) return Fail(diagnostic, "C# has no applied generation; build scripts in the Editor first");
        while (!generation.empty() && (generation.back() == '\n' || generation.back() == '\r')) generation.pop_back();
        if (!Leaf(generation) || !std::all_of(generation.begin(), generation.end(), [](unsigned char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') || value == '-';
        })) return Fail(diagnostic, "Invalid managed current.txt generation");
        resolved = output / generation / (project.stem().wstring() + L".dll");
    }
    if (!ValidateManagedGameFiles(resolved, diagnostic)) return false;
    assembly = std::move(resolved);
    return true;
}

bool InitializeManagedGame(const std::filesystem::path& root, const std::filesystem::path& hostDirectory,
    std::string_view configuration, bool& loaded, std::string& diagnostic)
{
    loaded = false;
    std::filesystem::path assembly;
    if (!ResolveManagedGameAssembly(root, configuration, assembly, diagnostic)) return false;
    if (assembly.empty()) return true;
    ManagedScriptRuntime& runtime = ManagedScriptRuntime::Instance();
    if (!runtime.Initialize(hostDirectory) || !runtime.Load(assembly)) return Fail(diagnostic, runtime.LastDiagnostic());
    loaded = true;
    diagnostic.clear();
    return true;
}
} /// @note namespace fbzz::scene
