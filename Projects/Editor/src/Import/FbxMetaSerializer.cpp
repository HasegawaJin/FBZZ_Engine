// FBZZ Engine
// FbxMetaSerializer.cpp | fbzz::editor
// FBX .meta のモデルインポート設定シリアライズ
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace fbzz::editor {
namespace {

const char* NormalMapConventionToString(NormalMapConvention value)
{
    switch (value) {
    case NormalMapConvention::DirectX: return "DirectX";
    case NormalMapConvention::OpenGL:  return "OpenGL";
    }
    return "DirectX";
}

NormalMapConvention StringToNormalMapConvention(std::string_view value)
{
    if (value == "OpenGL") return NormalMapConvention::OpenGL;
    return NormalMapConvention::DirectX;
}

const char* FbxSourceDccToString(FbxSourceDcc value)
{
    switch (value) {
    case FbxSourceDcc::Auto:    return "Auto";
    case FbxSourceDcc::Maya:    return "Maya";
    case FbxSourceDcc::Blender: return "Blender";
    }
    return "Auto";
}

FbxSourceDcc StringToFbxSourceDcc(std::string_view value)
{
    if (value == "Maya")    return FbxSourceDcc::Maya;
    if (value == "Blender") return FbxSourceDcc::Blender;
    return FbxSourceDcc::Auto;
}

const char* TextureCompressionToString(asset::TextureCompression value)
{
    switch (value) {
    case asset::TextureCompression::Auto: return "Auto";
    case asset::TextureCompression::BC1:  return "BC1";
    case asset::TextureCompression::BC3:  return "BC3";
    case asset::TextureCompression::BC4:  return "BC4";
    case asset::TextureCompression::BC5:  return "BC5";
    case asset::TextureCompression::BC6H: return "BC6H";
    case asset::TextureCompression::BC7:  return "BC7";
    case asset::TextureCompression::None: return "None";
    }
    return "Auto";
}

asset::TextureCompression StringToTextureCompression(std::string_view value)
{
    if (value == "BC1")  return asset::TextureCompression::BC1;
    if (value == "BC3")  return asset::TextureCompression::BC3;
    if (value == "BC4")  return asset::TextureCompression::BC4;
    if (value == "BC5")  return asset::TextureCompression::BC5;
    if (value == "BC6H") return asset::TextureCompression::BC6H;
    if (value == "BC7")  return asset::TextureCompression::BC7;
    if (value == "None") return asset::TextureCompression::None;
    return asset::TextureCompression::Auto;
}

toml::array ToTomlArray(const std::vector<std::string>& values)
{
    toml::array arr;
    for (const std::string& value : values)
        arr.push_back(value);
    return arr;
}

std::vector<std::string> ReadStringArray(const toml::array* arr)
{
    std::vector<std::string> values;
    if (!arr) return values;
    values.reserve(arr->size());
    for (const auto& item : *arr) {
        if (auto value = item.value<std::string>())
            values.push_back(*value);
    }
    return values;
}

uint64_t Fnv1a(std::string_view text, uint64_t hash = 1469598103934665603ull)
{
    for (unsigned char c : text) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex64(uint64_t value)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buf);
}

std::string ComputeSettingsHash(const FbxImportOptions& options)
{
    // インポータ版数をハッシュへ含める。これで版数を上げると settings_hash が変わり、
    // Library/Baked のコンテナも別キーになるので古い Bake が再利用されない。
    uint64_t hash = Fnv1a("iv:" + std::to_string(FbxMetaSerializer::kModelImporterVersion));
    hash = Fnv1a(FbxSourceDccToString(options.sourceDcc), hash);
    hash = Fnv1a(NormalMapConventionToString(options.normalMapConvention), hash);
    hash = Fnv1a(options.generateTexDescriptors ? "tex:1" : "tex:0", hash);
    hash = Fnv1a(TextureCompressionToString(options.defaultCompression), hash);
    for (const std::string& meshName : options.selectedMeshNames)
        hash = Fnv1a("mesh:" + meshName, hash);
    for (const std::string& animName : options.selectedAnimNames)
        hash = Fnv1a("anim:" + animName, hash);
    return Hex64(hash);
}

std::string ComputeSourceHash(const std::string& fbxAbsPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path = util::FileSystem::PathFromUtf8(fbxAbsPath);
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) return {};
    const auto writeTime = fs::last_write_time(path, ec);
    if (ec) return {};

    uint64_t hash = Fnv1a(util::FileSystem::NormalizePathSeparators(fbxAbsPath));
    hash = Fnv1a(std::to_string(size), hash);
    hash = Fnv1a(std::to_string(writeTime.time_since_epoch().count()), hash);
    return Hex64(hash);
}

toml::table LoadExistingRoot(const std::string& metaPath)
{
    toml::table root;
    std::string text;
    if (!util::FileSystem::ReadText(metaPath, text))
        return root;

    std::istringstream ss(text);
    const auto parsed = toml::parse(ss);
    if (parsed)
        root = parsed.table();
    return root;
}

bool WriteRoot(const std::string& metaPath, const toml::table& root)
{
    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(metaPath, ss.str());
}

void WriteOptionsToRoot(toml::table& root, const FbxImportOptions& options)
{
    toml::table model;
    model.insert("importer", "ModelImporter");
    model.insert("importer_version",
                 static_cast<int64_t>(FbxMetaSerializer::kModelImporterVersion));
    model.insert("source_dcc", FbxSourceDccToString(options.sourceDcc));
    model.insert("normal_map_convention", NormalMapConventionToString(options.normalMapConvention));
    model.insert("generate_tex_descriptors", options.generateTexDescriptors);
    model.insert("default_compression", TextureCompressionToString(options.defaultCompression));
    model.insert("selected_meshes", ToTomlArray(options.selectedMeshNames));
    model.insert("selected_animations", ToTomlArray(options.selectedAnimNames));

    root.insert_or_assign("file_format_version", static_cast<int64_t>(1));
    root.insert_or_assign("model", std::move(model));
}

} // namespace

std::string FbxMetaSerializer::MetaPathForSource(const std::string& fbxAbsPath)
{
    return fbxAbsPath + ".meta";
}

bool FbxMetaSerializer::LoadOptions(const std::string& fbxAbsPath, FbxImportOptions& outOptions)
{
    const toml::table root = LoadExistingRoot(MetaPathForSource(fbxAbsPath));
    const toml::table* model = root["model"].as_table();
    if (!model) return false;

    if (auto value = (*model)["source_dcc"].value<std::string>())
        outOptions.sourceDcc = StringToFbxSourceDcc(*value);
    if (auto value = (*model)["normal_map_convention"].value<std::string>())
        outOptions.normalMapConvention = StringToNormalMapConvention(*value);
    if (auto value = (*model)["generate_tex_descriptors"].value<bool>())
        outOptions.generateTexDescriptors = *value;
    if (auto value = (*model)["default_compression"].value<std::string>())
        outOptions.defaultCompression = StringToTextureCompression(*value);

    outOptions.selectedMeshNames = ReadStringArray((*model)["selected_meshes"].as_array());
    outOptions.selectedAnimNames = ReadStringArray((*model)["selected_animations"].as_array());
    return true;
}

int FbxMetaSerializer::LoadImporterVersion(const std::string& fbxAbsPath)
{
    const toml::table root = LoadExistingRoot(MetaPathForSource(fbxAbsPath));
    const toml::table* model = root["model"].as_table();
    if (!model) return 0;
    if (auto value = (*model)["importer_version"].value<int64_t>())
        return static_cast<int>(*value);
    return 0;
}

bool FbxMetaSerializer::SaveOptions(const std::string& fbxAbsPath, const FbxImportOptions& options)
{
    const std::string metaPath = MetaPathForSource(fbxAbsPath);
    toml::table root = LoadExistingRoot(metaPath);
    WriteOptionsToRoot(root, options);
    return WriteRoot(metaPath, root);
}

bool FbxMetaSerializer::SaveCacheInfo(const std::string& fbxAbsPath, const FbxImportOptions& options)
{
    const std::string metaPath = MetaPathForSource(fbxAbsPath);
    toml::table root = LoadExistingRoot(metaPath);
    WriteOptionsToRoot(root, options);

    toml::table cache;
    cache.insert("source_hash", ComputeSourceHash(fbxAbsPath));
    cache.insert("settings_hash", ComputeSettingsHash(options));
    cache.insert("baked_container", "Library/Baked/<asset-guid>");
    root.insert_or_assign("cache", std::move(cache));

    return WriteRoot(metaPath, root);
}

} // namespace fbzz::editor
