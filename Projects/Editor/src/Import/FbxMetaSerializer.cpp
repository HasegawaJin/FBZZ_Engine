/// @file    FbxMetaSerializer.cpp
/// @brief   FBX .meta のモデルインポート設定シリアライズ。
/// @author  Hasegawa Jin
/// @date    2026-07-08
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Import/ImportCacheStore.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Core/Logger.hpp>
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

// クリップ設定を TOML の配列テーブルへ。
//
// WHY 既定値のエントリを書かないか: FBX には数十本のクリップが入ることがあり、
//     全部を書くと .meta が「何も設定していないのに長大」になって差分が読めなくなる。
//     既定から外れたものだけを残せば、.meta を見ればどこを触ったかが分かる。
toml::array ToTomlArray(const std::vector<AnimationClipImportSettings>& clips)
{
    toml::array arr;
    for (const AnimationClipImportSettings& clip : clips) {
        if (clip.name.empty()) continue;
        if (!clip.loop && clip.startFrame == 0.0 && clip.endFrame < 0.0 && clip.outputName.empty())
            continue; // 既定と同じなら省略
        toml::table entry;
        entry.insert("name", clip.name);
        entry.insert("loop", clip.loop);
        entry.insert("start_frame", clip.startFrame);
        entry.insert("end_frame", clip.endFrame);
        entry.insert("output_name", clip.outputName);
        arr.push_back(std::move(entry));
    }
    return arr;
}

std::vector<AnimationClipImportSettings> ReadClipSettings(const toml::array* arr)
{
    std::vector<AnimationClipImportSettings> clips;
    if (!arr) return clips;
    clips.reserve(arr->size());
    for (const auto& item : *arr) {
        const toml::table* entry = item.as_table();
        if (!entry) continue;
        AnimationClipImportSettings settings;
        if (auto value = (*entry)["name"].value<std::string>()) settings.name = *value;
        if (settings.name.empty()) continue;
        settings.loop = (*entry)["loop"].value_or(false);
        settings.startFrame = (*entry)["start_frame"].value_or(0.0);
        settings.endFrame = (*entry)["end_frame"].value_or(-1.0);
        settings.outputName = (*entry)["output_name"].value_or(std::string{});
        clips.push_back(std::move(settings));
    }
    return clips;
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
    hash = Fnv1a(std::to_string(static_cast<int>(options.upAxis)), hash);
    hash = Fnv1a(NormalMapConventionToString(options.normalMapConvention), hash);
    hash = Fnv1a(std::to_string(options.unitScaleMultiplier), hash);
    hash = Fnv1a(options.generateNormals ? "normals:1" : "normals:0", hash);
    hash = Fnv1a(options.generateTangents ? "tangents:1" : "tangents:0", hash);
    hash = Fnv1a(options.generateTexDescriptors ? "tex:1" : "tex:0", hash);
    hash = Fnv1a(TextureCompressionToString(options.defaultCompression), hash);
    hash = Fnv1a("root_motion:" + options.rootMotionNodeName, hash);
    for (const std::string& meshName : options.selectedMeshNames)
        hash = Fnv1a("mesh:" + meshName, hash);
    for (const std::string& animName : options.selectedAnimNames)
        hash = Fnv1a("anim:" + animName, hash);
    // クリップ設定もハッシュへ含める。これで Loop Time を切り替えるだけで
    // settings_hash が変わり、既存の再インポート判定がそのまま走る
    // (専用の「再インポートが要るか」判定を足さなくて済む)。
    for (const AnimationClipImportSettings& clip : options.clipSettings) {
        // 既定値のクリップは .meta にもハッシュにも不要。
        if (!clip.loop && clip.startFrame == 0.0 && clip.endFrame < 0.0 && clip.outputName.empty())
            continue;
        hash = Fnv1a("clip:" + clip.name, hash);
        hash = Fnv1a(clip.loop ? "loop:1" : "loop:0", hash);
        hash = Fnv1a(std::to_string(clip.startFrame), hash);
        hash = Fnv1a(std::to_string(clip.endFrame), hash);
        hash = Fnv1a(clip.outputName, hash);
    }
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
    model.insert("up_axis", static_cast<int64_t>(options.upAxis));
    model.insert("normal_map_convention", NormalMapConventionToString(options.normalMapConvention));
    model.insert("unit_scale_multiplier", options.unitScaleMultiplier);
    model.insert("generate_normals", options.generateNormals);
    model.insert("generate_tangents", options.generateTangents);
    model.insert("generate_tex_descriptors", options.generateTexDescriptors);
    model.insert("default_compression", TextureCompressionToString(options.defaultCompression));
    model.insert("root_motion_node", options.rootMotionNodeName);
    model.insert("selected_meshes", ToTomlArray(options.selectedMeshNames));
    model.insert("selected_animations", ToTomlArray(options.selectedAnimNames));
    model.insert("clips", ToTomlArray(options.clipSettings));

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
    if (auto value = (*model)["up_axis"].value<int64_t>())
        outOptions.upAxis = static_cast<FbxUpAxis>(*value);
    if (auto value = (*model)["normal_map_convention"].value<std::string>())
        outOptions.normalMapConvention = StringToNormalMapConvention(*value);
    // toml++ は保存時の C++ 型を保持するため、float で保存した既存 .meta は
    // value<double>() では取得できない。float / double の両方を受け入れ、
    // 保存形式に依存せず FBXImport へ設定値を渡す。
    if (auto value = (*model)["unit_scale_multiplier"].value<float>())
        outOptions.unitScaleMultiplier = *value;
    else if (auto value = (*model)["unit_scale_multiplier"].value<double>())
        outOptions.unitScaleMultiplier = static_cast<float>(*value);
    if (auto value = (*model)["generate_normals"].value<bool>())
        outOptions.generateNormals = *value;
    if (auto value = (*model)["generate_tangents"].value<bool>())
        outOptions.generateTangents = *value;
    if (auto value = (*model)["generate_tex_descriptors"].value<bool>())
        outOptions.generateTexDescriptors = *value;
    if (auto value = (*model)["default_compression"].value<std::string>())
        outOptions.defaultCompression = StringToTextureCompression(*value);
    if (auto value = (*model)["root_motion_node"].value<std::string>())
        outOptions.rootMotionNodeName = *value;

    outOptions.selectedMeshNames = ReadStringArray((*model)["selected_meshes"].as_array());
    outOptions.selectedAnimNames = ReadStringArray((*model)["selected_animations"].as_array());
    outOptions.clipSettings      = ReadClipSettings((*model)["clips"].as_array());
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
    // guid の確定を .meta の読み込みより先に済ませる。
    // WHY: GuidFromPath は guid が無ければ .meta を書いて発行する。後から呼ぶと、
    //      その書き込み前に読んだ root で上書きしてしまい、発行した guid が消える。
    const std::string guid = asset::AssetDatabase::GuidFromPath(fbxAbsPath);

    ImportCacheStore::Entry entry;
    entry.sourceHash   = ComputeSourceHash(fbxAbsPath);
    entry.settingsHash = ComputeSettingsHash(options);
    if (!ImportCacheStore::Save(guid, entry)) {
        // 記録できないと IsOutdated が mtime 比較へ落ちるだけで、import 自体は成功している。
        FBZZ_LOG_WARN("FbxMetaSerializer: import cache not recorded [%s]", fbxAbsPath.c_str());
    }

    const std::string metaPath = MetaPathForSource(fbxAbsPath);
    toml::table root = LoadExistingRoot(metaPath);
    WriteOptionsToRoot(root, options);
    // 旧形式で .meta へ焼かれていた [cache] を落とし、保管場所を Library へ一本化する。
    root.erase("cache");

    return WriteRoot(metaPath, root);
}

FbxMetaSerializer::CacheInfo FbxMetaSerializer::LoadCacheInfo(const std::string& fbxAbsPath)
{
    // TryGetGuidFromPath を使う。判定のためだけに未 import の FBX へ .meta を発行しない。
    const ImportCacheStore::Entry entry =
        ImportCacheStore::Load(asset::AssetDatabase::TryGetGuidFromPath(fbxAbsPath));
    if (!entry.Empty()) return { entry.sourceHash, entry.settingsHash };

    // 移行フォールバック: Library へ移す前は .meta の [cache] に焼いていた。
    // 記録が残っていれば読み、既存プロジェクトを丸ごと焼き直さずに済ませる。
    // 次の import で [cache] は落ちるので、以降この経路は通らない。
    CacheInfo info;
    const toml::table root = LoadExistingRoot(MetaPathForSource(fbxAbsPath));
    const toml::table* cache = root["cache"].as_table();
    if (!cache) return info;
    info.sourceHash   = (*cache)["source_hash"].value_or(std::string{});
    info.settingsHash = (*cache)["settings_hash"].value_or(std::string{});
    return info;
}

std::string FbxMetaSerializer::SourceHash(const std::string& fbxAbsPath)
{
    return ComputeSourceHash(fbxAbsPath);
}

std::string FbxMetaSerializer::SettingsHash(const FbxImportOptions& options)
{
    return ComputeSettingsHash(options);
}

} // namespace fbzz::editor
