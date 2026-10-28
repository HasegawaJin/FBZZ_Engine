/// @file    ManagedSdkBinding.cpp
/// @brief   配布manifestのvalidated構成とhashに結び付け、ancestor repositoryへの誤fallbackを拒否する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Editor/Util/ManagedSdkBinding.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <array>
#include <fstream>
#include <algorithm>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
namespace fbzz::editor {
namespace {
bool Fail(std::string& reason, std::string value) { reason = std::move(value); return false; }
bool Regular(const std::filesystem::path& path)
{ std::error_code error; return std::filesystem::is_regular_file(path, error) && !error; }
std::filesystem::path Canonical(const std::filesystem::path& path)
{ std::error_code error; auto result = std::filesystem::weakly_canonical(path, error); return error ? std::filesystem::path{} : result; }
bool Equal(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return !left.empty() && !right.empty() && CompareStringOrdinal(left.c_str(), static_cast<int>(left.native().size()),
        right.c_str(), static_cast<int>(right.native().size()), TRUE) == CSTR_EQUAL;
}
std::string Config(const std::filesystem::path& value)
{
    for (const char* name : {"Debug", "Development", "Release"}) if (Equal(value, name)) return name;
    return {};
}
std::filesystem::path Repository(std::filesystem::path root)
{
    while (!root.empty()) {
        if (Regular(root / "Tools/AgentBuild.ps1") && Regular(root / "CMakeLists.txt")) return root;
        const auto parent = root.parent_path(); if (parent == root) break; root = parent;
    }
    return {};
}
/// @note CNGが所有するhashを固定64KiBで更新する。SDK DLL全体を一時配列へ複製しない。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash BCryptCreateHash buffer ownership。
bool Hash(const std::filesystem::path& path, std::string& out)
{
    struct Handles { BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        ~Handles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); } } handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    std::ifstream file(path, std::ios::binary); if (!file) return false;
    std::array<unsigned char, 65536> bytes{};
    while (file) { file.read(static_cast<char*>(static_cast<void*>(bytes.data())), static_cast<std::streamsize>(bytes.size())); const auto count = file.gcount();
        if (count && BCryptHashData(handles.hash, bytes.data(), static_cast<ULONG>(count), 0) < 0) return false; }
    if (!file.eof()) return false;
    std::array<unsigned char, 32> digest{}; if (BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char hex[] = "0123456789abcdef"; out.clear(); out.reserve(64);
    for (auto value : digest) { out.push_back(hex[value >> 4]); out.push_back(hex[value & 15]); } return true;
}
bool ValidHash(const std::string& hash)
{ return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }); }
bool ValidateSdk(const ManagedSdkBinding& binding, const std::filesystem::path& executable, std::string& reason)
{
    std::string text; const auto path = binding.root / "fbzz-sdk.toml";
    if (!util::FileSystem::ReadText(path, text)) return Fail(reason, "SDK manifest unavailable: " + util::FileSystem::PathToUtf8(path));
    const auto parsed = toml::parse(text); if (!parsed || parsed.table()["sdk"]["schema"].value_or(0) != 2) return Fail(reason, "Unsupported SDK manifest schema");
    const auto& manifest = parsed.table(); const auto configuration = manifest["configurations"][binding.configuration];
    const auto fingerprint = manifest["runtime"]["fingerprint"].value_or(std::string{});
    if (!ValidHash(fingerprint) || !configuration["validated"].value_or(false)
        || configuration["fingerprint"].value_or(std::string{}) != fingerprint) return Fail(reason, "SDK configuration is not validated: " + binding.configuration);
    const auto files = manifest["files"][binding.configuration];
    const std::string editorPrefix = "tools/" + binding.configuration + "/Editor/", binPrefix = "bin/" + binding.configuration + "/";
    const auto check = [&](const std::string& relative) {
        const auto expected = files[relative].value_or(std::string{}); std::string actual;
        if (!ValidHash(expected) || !Hash(binding.root / util::FileSystem::PathFromUtf8(relative), actual) || actual != expected)
            return Fail(reason, "SDK managed publication mismatch: " + relative);
        return true;
    };
    if (!Equal(executable, Canonical(binding.root / util::FileSystem::PathFromUtf8(editorPrefix + "FBZZEditor.exe")))) return Fail(reason, "SDK executable does not match its published layout");
    if (!check(editorPrefix + "FBZZEditor.exe")) return false;
    for (const std::string relative : std::array<std::string, 9>{"nethost.dll", "Managed/Fbzz.ScriptApi.dll", "Managed/Fbzz.ScriptApi.xml", "Managed/Fbzz.ScriptGenerator.dll",
        "Managed/Fbzz.ManagedBuildTasks.dll", "Managed/ManagedIncremental.targets", "Managed/Fbzz.ManagedHost.dll", "Managed/Fbzz.ManagedHost.runtimeconfig.json", "Managed/Fbzz.ManagedHost.deps.json"}) {
        const auto first = files[binPrefix + relative].value_or(std::string{}), second = files[editorPrefix + relative].value_or(std::string{});
        if (!ValidHash(first) || first != second) return Fail(reason, "SDK managed pair records disagree: " + relative);
        if (!check(binPrefix + relative) || !check(editorPrefix + relative)) return false;
    }
    constexpr std::array<const char*, 7> shared{"Tools/AgentBuild.ps1", "Projects/Managed/global.json", "share/fbzz/managed/ManagedStarter/Starter.Scripts.csproj",
        "share/fbzz/managed/ManagedStarter/Assets/Scripts/Managed/MoveForwardComponent.cs", "share/fbzz/managed/ManagedStarter/AGENTS.md",
        "share/fbzz/managed/ManagedStarter/.vscode/launch.json", "share/fbzz/managed/ManagedStarter/global.json"};
    if (files["Projects/Managed/global.json"].value_or(std::string{}) != files["share/fbzz/managed/ManagedStarter/global.json"].value_or(std::string{})) return Fail(reason, "SDK compiler pin records disagree");
    for (const char* relative : shared) if (!check(relative)) return false;
    return true;
}
} /// @note namespace
std::filesystem::path ManagedEditorExecutable()
{
    std::wstring buffer(1024, L'\0');
    for (;;) { const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size) return {}; if (size < buffer.size()) { buffer.resize(size); return Canonical(buffer); }
        if (buffer.size() >= 32768) return {}; buffer.resize(buffer.size() * 2); }
}
bool ResolveManagedSdkBinding(const std::filesystem::path& executableInput, const std::filesystem::path& project,
    const std::filesystem::path& explicitRoot, const std::string& sourceConfiguration, ManagedSdkBinding& out, std::string& diagnostic)
{
    const auto executable = Canonical(executableInput); if (executable.empty() || !Regular(executable)) return Fail(diagnostic, "Editor executable is unavailable");
    ManagedSdkBinding binding; binding.configuration = sourceConfiguration;
    const auto directory = executable.parent_path(), configuration = directory.parent_path(), tools = configuration.parent_path();
    const std::string config = Config(configuration.filename());
    const bool layout = Equal(directory.filename(), "Editor") && Equal(tools.filename(), "tools") && !config.empty();
    std::filesystem::path sdk = layout ? tools.parent_path() : std::filesystem::path{};
    if (sdk.empty()) for (auto parent = directory; !parent.empty();) {
        if (Regular(parent / "fbzz-sdk.toml")) { sdk = parent; break; }
        const auto next = parent.parent_path(); if (next == parent) break; parent = next;
    }
    auto chosen = explicitRoot.empty() ? std::filesystem::path{} : Canonical(explicitRoot.is_absolute() ? explicitRoot : project / explicitRoot);
    if (!explicitRoot.empty() && chosen.empty()) return Fail(diagnostic, "Explicit SDK root is unavailable");
    if (!sdk.empty()) {
        binding.published = true; binding.root = Canonical(sdk); binding.configuration = layout ? config : sourceConfiguration;
        if (!chosen.empty() && !Equal(chosen, binding.root)) return Fail(diagnostic, "Explicit SDK root does not match the running published Editor");
        binding.agentBuild = binding.root / "Tools/AgentBuild.ps1"; binding.hostDirectory = directory / "Managed"; binding.apiPath = binding.hostDirectory / "Fbzz.ScriptApi.dll";
        if (!ValidateSdk(binding, executable, diagnostic)) return false;
    } else {
        auto discovered = Repository(directory);
        if (!chosen.empty()) {
            if (Regular(chosen / "fbzz-sdk.toml")) return Fail(diagnostic, "Explicit SDK root does not contain the running Editor");
            if (!discovered.empty() && !Equal(discovered, chosen)) return Fail(diagnostic, "Explicit repository root does not match the running source Editor");
            if (!Regular(chosen / "Tools/AgentBuild.ps1") || !Regular(chosen / "CMakeLists.txt")) return Fail(diagnostic, "Source Editor repository build entry is unavailable");
            discovered = chosen;
        }
        if (discovered.empty()) discovered = Repository(Canonical(project));
        binding.configuration = Config(sourceConfiguration);
        if (discovered.empty() || binding.configuration.empty()) return Fail(diagnostic, "Source Editor repository/configuration is unavailable");
        binding.root = discovered; binding.agentBuild = discovered / "Tools/AgentBuild.ps1";
        for (const auto& host : std::array<std::filesystem::path, 3>{directory / "Managed", directory.parent_path() / "Managed", discovered / "bin" / sourceConfiguration / "Managed"})
            if (Regular(host / "Fbzz.ManagedHost.dll") && Regular(host / "Fbzz.ScriptApi.dll")) { binding.hostDirectory = host; break; }
        if (binding.hostDirectory.empty()) binding.hostDirectory = discovered / "bin" / binding.configuration / "Managed";
        binding.apiPath = binding.hostDirectory / "Fbzz.ScriptApi.dll";
    }
    diagnostic.clear(); out = std::move(binding); return true;
}
bool ResolveProjectManagedSdkBinding(const std::filesystem::path& executable, const std::filesystem::path& project,
    const std::filesystem::path& environmentRoot, const std::string& sourceConfiguration, ManagedSdkBinding& out, std::string& diagnostic)
{
    std::filesystem::path explicitRoot = environmentRoot;
    if (explicitRoot.empty()) {
        std::string text;
        if (util::FileSystem::ReadText(project / ".fbzz_proj", text)) {
            const auto parsed = toml::parse(text); if (!parsed) return Fail(diagnostic, "Project metadata is invalid");
            const std::string value = parsed.table()["engine"]["sdk_root"].value_or(parsed.table()["engine"]["root"].value_or(std::string{}));
            if (!value.starts_with("{{")) explicitRoot = util::FileSystem::PathFromUtf8(value);
        }
    }
    return ResolveManagedSdkBinding(executable, project, explicitRoot, sourceConfiguration, out, diagnostic);
}
} /// @note namespace fbzz::editor
