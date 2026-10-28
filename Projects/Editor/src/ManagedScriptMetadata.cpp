/// @file    ManagedScriptMetadata.cpp
/// @brief   .cs.meta の永続型 GUID を宣言 checksum と照合して候補へ記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedScriptMetadata.hpp"
#include <Editor/Ai/Json.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fbzz::editor {
namespace {

using ai::JsonValue;

struct MetadataFile {
    std::filesystem::path path;
    std::string relativeSource;
    std::string original;
    toml::table root;
    bool existed = false;
    bool changed = false;
};

struct TypeMapping {
    std::size_t file = 0;
    std::size_t row = 0;
    std::string guid;
    std::string clrName;
};

/// @note 登録済み型の表示名は承認済み catalog に属し、候補の Publish 成功時だけ置き換える。
void SeedUnregisteredDisplayNames(const std::vector<TypeMapping>& mappings)
{
    const std::vector<std::string> registered = scene::ScriptFactory::RegisteredTypeNames();
    for (const TypeMapping& mapping : mappings) {
        const std::string typeKey = "cs:" + mapping.guid;
        if (!std::binary_search(registered.begin(), registered.end(), typeKey))
            scene::ScriptFactory::SetDisplayName(typeKey, mapping.clrName);
    }
}

bool IsHex(const std::string& value, std::size_t length)
{
    return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

std::string LowerHex(std::string value)
{
    for (char& character : value)
        if (character >= 'A' && character <= 'F') character = static_cast<char>(character + ('a' - 'A'));
    return value;
}

bool IsScriptMetadataPath(const std::filesystem::path& path)
{
    std::string name = util::FileSystem::PathToUtf8(path.filename());
    for (char& character : name)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return name.ends_with(".cs.meta");
}

/// @note CNG の管理バッファを使い、Roslyn が読んだ original source bytes の SHA256 と照合する。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash BCryptCreateHash buffer ownership
bool SourceSha256(const std::filesystem::path& path, std::string& result)
{
    std::vector<std::uint8_t> bytes;
    if (!util::FileSystem::ReadBinary(path, bytes)) return false;
    struct HashHandles {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~HashHandles()
        {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const ULONG size = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
        if (BCryptHashData(handles.hash, bytes.data() + offset, size, 0) < 0) return false;
        offset += size;
    }
    std::array<std::uint8_t, 32> digest{};
    if (BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char HEX[] = "0123456789abcdef";
    result.clear();
    for (const std::uint8_t value : digest) {
        result.push_back(HEX[value >> 4]);
        result.push_back(HEX[value & 15]);
    }
    return true;
}

bool IsSourcePath(const std::string& relative)
{
    const std::filesystem::path path = util::FileSystem::PathFromUtf8(relative);
    std::string extension = util::FileSystem::PathToUtf8(path.extension());
    for (char& character : extension)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    if (!relative.starts_with("Assets/") || extension != ".cs" ||
        relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos) return false;
    if (path.is_absolute()) return false;
    for (const std::filesystem::path& part : path)
        if (part == "." || part == ".." || part.empty()) return false;
    return true;
}

bool IsUnlinkedSource(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path relative = path.lexically_relative(root);
    std::filesystem::path current = root;
    for (const std::filesystem::path& part : relative) {
        current /= part;
        if (std::filesystem::is_symlink(current, error) || error) return false;
    }
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool ReadMetadata(const std::filesystem::path& path, const std::filesystem::path& root,
    MetadataFile& file, std::string& diagnostic)
{
    file.path = path;
    std::filesystem::path source = path;
    source.replace_extension();
    file.relativeSource = util::FileSystem::NormalizePathSeparators(
        util::FileSystem::PathToUtf8(source.lexically_relative(root)));
    std::error_code error;
    file.existed = std::filesystem::exists(path, error);
    if (error || (file.existed && std::filesystem::is_symlink(path, error)) || error) {
        diagnostic = "Script metadata must be a regular project file: " + file.relativeSource + ".meta";
        return false;
    }
    if (file.existed) {
        if (!util::FileSystem::ReadText(path, file.original)) {
            diagnostic = "Script metadata could not be read: " + file.relativeSource + ".meta";
            return false;
        }
        const toml::parse_result parsed = toml::parse(file.original);
        if (!parsed) {
            diagnostic = "Malformed .cs.meta must be repaired before applying scripts: " + file.relativeSource;
            return false;
        }
        file.root = parsed.table();
        const std::string assetGuid = LowerHex(file.root["meta"]["guid"].value_or(std::string{}));
        if (!IsHex(assetGuid, 32)) {
            diagnostic = "Existing .cs.meta requires its original [meta] guid: " + file.relativeSource;
            return false;
        }
    } else {
        toml::table meta;
        meta.insert("guid", asset::AssetDatabase::GenerateGuid());
        file.root.insert("meta", std::move(meta));
        file.changed = true;
    }
    if (file.root.contains("managed") && !file.root["managed"].is_table()) {
        diagnostic = "Expected [managed] table in " + file.relativeSource + ".meta";
        return false;
    }
    if (file.root["managed"]["types"] && !file.root["managed"]["types"].is_array()) {
        diagnostic = "Expected [[managed.types]] array in " + file.relativeSource + ".meta";
        return false;
    }
    return true;
}

toml::array& TypeRows(MetadataFile& file)
{
    if (!file.root["managed"].is_table()) file.root.insert("managed", toml::table{});
    toml::table& managed = *file.root["managed"].as_table();
    if (!managed["types"].is_array()) managed.insert("types", toml::array{});
    return *managed["types"].as_array();
}

bool CollectMappings(std::vector<MetadataFile>& files, std::vector<TypeMapping>& mappings, std::string& diagnostic)
{
    std::set<std::string> guids;
    std::set<std::string> names;
    std::map<std::string, std::string> nameOwners;
    for (std::size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const MetadataFile& file = files[fileIndex];
        const toml::array* rows = file.root["managed"]["types"].as_array();
        if (!rows) continue;
        for (std::size_t rowIndex = 0; rowIndex < rows->size(); ++rowIndex) {
            const toml::table* row = (*rows)[rowIndex].as_table();
            const std::string guid = row ? LowerHex((*row)["guid"].value_or(std::string{})) : "";
            const std::string clrName = row ? (*row)["clrName"].value_or(std::string{}) : "";
            if (!row || !IsHex(guid, 32) || clrName.empty() || !guids.insert(guid).second || !names.insert(clrName).second) {
                diagnostic = "Invalid or duplicate managed type guid / clrName in " + file.relativeSource + ".meta";
                return false;
            }
            if (const auto found = nameOwners.find(clrName); found != nameOwners.end() && found->second != guid) {
                diagnostic = "Ambiguous current / previous CLR name in " + file.relativeSource + ".meta: " + clrName;
                return false;
            }
            nameOwners[clrName] = guid;
            if ((*row)["previousClrNames"] && !(*row)["previousClrNames"].is_array()) {
                diagnostic = "previousClrNames must be an array in " + file.relativeSource + ".meta";
                return false;
            }
            if (const toml::array* previous = (*row)["previousClrNames"].as_array()) {
                std::set<std::string> aliases;
                for (const toml::node& alias : *previous) {
                    const std::string name = alias.value_or(std::string{});
                    if (name.empty() || name == clrName || !aliases.insert(name).second) {
                        diagnostic = "Invalid previousClrNames in " + file.relativeSource + ".meta";
                        return false;
                    }
                    if (const auto found = nameOwners.find(name); found != nameOwners.end() && found->second != guid) {
                        diagnostic = "Ambiguous previous CLR name in " + file.relativeSource + ".meta: " + name;
                        return false;
                    }
                    nameOwners[name] = guid;
                }
            }
            mappings.push_back({ fileIndex, rowIndex, guid, clrName });
        }
    }
    return true;
}

bool CommitFiles(std::vector<MetadataFile>& files, std::string& diagnostic)
{
    for (const MetadataFile& file : files) {
        std::string current;
        std::error_code error;
        const bool exists = std::filesystem::exists(file.path, error);
        if (error || exists != file.existed ||
            (exists && (!util::FileSystem::ReadText(file.path, current) || current != file.original))) {
            diagnostic = "Script metadata changed during import: " + file.relativeSource + ".meta";
            return false;
        }
    }
    std::vector<std::size_t> committed;
    for (std::size_t index = 0; index < files.size(); ++index) {
        MetadataFile& file = files[index];
        if (!file.changed) continue;
        std::ostringstream stream;
        stream << file.root;
        if (util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(file.path), stream.str())) {
            committed.push_back(index);
            continue;
        }
        bool restored = true;
        for (const std::size_t previous : committed) {
            MetadataFile& written = files[previous];
            if (written.existed)
                restored = util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(written.path), written.original) && restored;
            else {
                std::error_code error;
                restored = std::filesystem::remove(written.path, error) && !error && restored;
            }
        }
        diagnostic = "Unable to write Script metadata: " + file.relativeSource + (restored ? " (previous files restored)" : " (rollback incomplete)");
        return false;
    }
    return true;
}

} /// @note namespace

bool LoadManagedScriptDisplayNames(const std::filesystem::path& projectRoot, std::string& diagnostic)
{
    diagnostic.clear();
    std::vector<MetadataFile> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(projectRoot / "Assets", error);
    if (error) { diagnostic = "Cannot inspect project Script metadata"; return false; }
    for (; iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error)) {
        if (error) break;
        if (iterator->is_symlink(error)) { iterator.disable_recursion_pending(); continue; }
        if (error) break;
        const std::filesystem::path path = iterator->path();
        if (!iterator->is_regular_file(error) || !IsScriptMetadataPath(path)) continue;
        MetadataFile file;
        if (!ReadMetadata(path, projectRoot, file, diagnostic)) return false;
        files.push_back(std::move(file));
    }
    if (error) { diagnostic = "Cannot inspect project Script metadata"; return false; }
    std::vector<TypeMapping> mappings;
    if (!CollectMappings(files, mappings, diagnostic)) return false;
    SeedUnregisteredDisplayNames(mappings);
    return true;
}

bool PrepareManagedScriptManifest(const std::filesystem::path& projectRoot,
    const std::filesystem::path& assembly, const std::string& discoveredSources, std::string& diagnostic)
{
    diagnostic.clear();
    const std::optional<JsonValue> catalog = ai::ParseJson(discoveredSources, &diagnostic);
    if (!catalog || !catalog->IsArray()) {
        diagnostic = "Invalid compiled Script source catalog: " + diagnostic;
        return false;
    }
    std::vector<MetadataFile> files;
    std::map<std::string, std::size_t> fileBySource;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(projectRoot / "Assets", error);
    if (error) { diagnostic = "Cannot inspect project Script metadata"; return false; }
    for (; iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error)) {
        if (error) break;
        if (iterator->is_symlink(error)) { iterator.disable_recursion_pending(); continue; }
        if (error) break;
        const std::filesystem::path path = iterator->path();
        if (!iterator->is_regular_file(error) || !IsScriptMetadataPath(path)) continue;
        MetadataFile file;
        if (!ReadMetadata(path, projectRoot, file, diagnostic)) return false;
        fileBySource.emplace(file.relativeSource, files.size());
        files.push_back(std::move(file));
    }
    if (error) { diagnostic = "Cannot inspect project Script metadata"; return false; }
    std::vector<TypeMapping> mappings;
    if (!CollectMappings(files, mappings, diagnostic)) return false;
    JsonValue manifest = JsonValue::MakeObject();
    manifest.Set("schemaVersion", JsonValue(1));
    manifest.Set("assemblyName", JsonValue(util::FileSystem::PathToUtf8(assembly.stem())));
    JsonValue types = JsonValue::MakeArray();
    std::set<std::string> currentNames;
    std::map<std::string, std::string> sourceHashes;
    for (const JsonValue& discovered : catalog->AsArray()) {
        const JsonValue* name = discovered.Find("clrName");
        const JsonValue* sources = discovered.Find("sources");
        if (!name || !name->IsString() || name->AsString().empty() || !currentNames.insert(name->AsString()).second ||
            !sources || !sources->IsArray() || sources->AsArray().empty()) {
            diagnostic = "Compiled Script requires a unique clrName and nonempty declarations";
            return false;
        }
        std::set<std::string> sourcePaths;
        std::set<std::pair<std::string, int>> declarations;
        for (const JsonValue& source : sources->AsArray()) {
            const JsonValue* path = source.Find("path");
            const JsonValue* line = source.Find("line");
            const JsonValue* hash = source.Find("contentHash");
            if (!path || !path->IsString() || !IsSourcePath(path->AsString()) || !line || !line->IsNumber() ||
                line->AsNumber() < 1 || line->AsNumber() > 2147483647.0 || std::floor(line->AsNumber()) != line->AsNumber() ||
                !hash || !hash->IsString() || !IsHex(hash->AsString(), 64) ||
                !declarations.emplace(path->AsString(), line->AsInt()).second) {
                diagnostic = "Invalid compiled Script declaration for " + name->AsString();
                return false;
            }
            const std::filesystem::path absolute = projectRoot / util::FileSystem::PathFromUtf8(path->AsString());
            if (!IsUnlinkedSource(projectRoot, absolute)) {
                diagnostic = "Script declaration is absent or linked outside the project: " + path->AsString();
                return false;
            }
            auto actual = sourceHashes.find(path->AsString());
            if (actual == sourceHashes.end()) {
                std::string actualHash;
                if (!SourceSha256(absolute, actualHash)) { diagnostic = "Cannot hash Script source " + path->AsString(); return false; }
                actual = sourceHashes.emplace(path->AsString(), std::move(actualHash)).first;
            }
            if (actual->second != hash->AsString()) {
                diagnostic = "Compiled source checksum differs from disk: " + path->AsString();
                return false;
            }
            sourcePaths.insert(path->AsString());
            if (!fileBySource.contains(path->AsString())) {
                MetadataFile file;
                if (!ReadMetadata(std::filesystem::path(absolute.wstring() + L".meta"), projectRoot, file, diagnostic)) return false;
                fileBySource.emplace(file.relativeSource, files.size());
                files.push_back(std::move(file));
            }
        }
        const TypeMapping* chosen = nullptr;
        for (const TypeMapping& mapping : mappings) {
            if (mapping.clrName != name->AsString()) continue;
            if (!sourcePaths.contains(files[mapping.file].relativeSource)) {
                diagnostic = "Move the existing type mapping to a current declaration .cs.meta: " + name->AsString();
                return false;
            }
            chosen = &mapping;
        }
        if (!chosen && sourcePaths.size() != 1) {
            diagnostic = "Partial Script requires an explicit primary [[managed.types]] mapping in one declaration .cs.meta: " + name->AsString();
            return false;
        }
        if (!chosen) {
            const std::size_t fileIndex = fileBySource.at(*sourcePaths.begin());
            MetadataFile& file = files[fileIndex];
            toml::array& rows = TypeRows(file);
            toml::table row;
            const std::string guid = asset::AssetDatabase::GenerateGuid();
            row.insert("guid", guid);
            row.insert("clrName", name->AsString());
            rows.push_back(std::move(row));
            file.changed = true;
            mappings.push_back({ fileIndex, rows.size() - 1, guid, name->AsString() });
            chosen = &mappings.back();
        }
        JsonValue type = JsonValue::MakeObject();
        type.Set("id", JsonValue("cs:" + chosen->guid));
        type.Set("clrName", JsonValue(chosen->clrName));
        type.Set("primarySource", JsonValue(files[chosen->file].relativeSource));
        type.Set("sources", *sources);
        types.Push(std::move(type));
    }
    manifest.Set("types", std::move(types));
    std::vector<TypeMapping> verifiedMappings;
    if (!CollectMappings(files, verifiedMappings, diagnostic)) return false;
    if (!CommitFiles(files, diagnostic)) return false;
    const std::filesystem::path sidecar = std::filesystem::path(assembly.wstring() + L".scripts.json");
    if (!util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(sidecar), ai::SerializeJson(manifest))) {
        diagnostic = "Cannot write candidate Script GUID manifest";
        return false;
    }
    SeedUnregisteredDisplayNames(verifiedMappings);
    return true;
}

} /// @note namespace fbzz::editor
