/// @file    AssetBrowserOperators.cpp
/// @brief   Asset Browser のナビゲーション、表示設定、アセット作成を公開する。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note AssetBrowser の一覧・検査・サムネイルは AI Query から既に使えるが、参照確認や再インポート設定を
/// @note 開く検証導線は UI だけに閉じていた。EditorContext の one-shot request を共有し、パネルの所有権や
/// @note ファイル監視の実装を Operator 層へ漏らさない。
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Asset/Browser/AssetBrowserFolders.hpp>
#include <Editor/Script/Authoring/ScriptCodeGen.hpp>
#include <Editor/Script/Managed/ManagedDataAssetEditor.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Util/Files/FileSystem.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <optional>
#include <system_error>
#include <utility>

namespace fbzz::editor {

std::string FormatAssetBrowserFolderColor(std::uint32_t color)
{
    char text[8];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X",
                  (color >> IM_COL32_R_SHIFT) & 0xFFu,
                  (color >> IM_COL32_G_SHIFT) & 0xFFu,
                  (color >> IM_COL32_B_SHIFT) & 0xFFu);
    return text;
}

namespace {

using util::FileSystem;
namespace fs = std::filesystem;

OpParam AssetPathParam(const char* description)
{
    OpParam param;
    param.name     = "path";
    param.type     = OpParamType::String;
    param.desc     = description;
    param.required = true;
    return param;
}

struct ColorFolder {
    fs::path path;
    fs::path canonicalPath;
};

std::optional<ColorFolder> ResolveColorFolder(const EditorContext& ctx, const std::string& input)
{
    if (ctx.projectRoot.empty() || input.empty() || input.find('\0') != std::string::npos)
        return std::nullopt;
    const auto project = FileSystem::PathFromUtf8(ctx.projectRoot);
    auto path = FileSystem::PathFromUtf8(input);
    if (!path.is_absolute()) {
        if (path.has_root_path()) return std::nullopt;
        path = project / path;
    }
    const auto absolute = FileSystem::NormalizePathSeparators(FileSystem::PathToUtf8(path.lexically_normal()));
    path = FileSystem::PathFromUtf8(absolute);
    std::error_code error;
    if (!fs::is_directory(path, error)) return std::nullopt;
    const auto canonicalPath = fs::canonical(path, error);
    if (error) return std::nullopt;

    const auto assetsRoot = FileSystem::PathToUtf8((project / "Assets").lexically_normal());
    std::string root = assetsRoot;
    if (!FileSystem::IsChildPathText(absolute, root)) {
        const auto mounts = ResolveAssetBrowserMounts(ctx, assetsRoot);
        const auto mount = std::find_if(mounts.begin(), mounts.end(), [&](const AssetMount& entry) {
            return FileSystem::IsChildPathText(absolute, entry.path);
        });
        if (mount == mounts.end()) return std::nullopt;
        root = mount->path;
    }

    const auto canonicalRoot = fs::canonical(FileSystem::PathFromUtf8(root), error);
    if (error || !FileSystem::IsChildPathText(FileSystem::PathToUtf8(canonicalPath),
                                              FileSystem::PathToUtf8(canonicalRoot)))
        return std::nullopt;

    /// @note 非表示のフォルダの配下も一覧とツリーから到達できない。
    for (auto current = path; !FileSystem::SamePathText(FileSystem::PathToUtf8(current), root);
         current = current.parent_path()) {
        if (!IsVisibleAssetBrowserFolder(FileSystem::PathToUtf8(current))) return std::nullopt;
    }
    return ColorFolder{std::move(path), canonicalPath};
}

std::optional<std::uint32_t> ParseFolderColor(const std::string& text)
{
    if (text.size() != 7 || text.front() != '#') return std::nullopt;
    unsigned rgb = 0;
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data() + 1, end, rgb, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    return IM_COL32((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu, 255u);
}

std::vector<std::string> CollectColorFolders(const ColorFolder& folder, bool recursive,
                                             std::error_code& error)
{
    std::vector<std::string> keys{AssetBrowserFolderKey(FileSystem::PathToUtf8(folder.path))};
    if (!recursive) return keys;
    for (fs::recursive_directory_iterator entry(folder.path, error);
         !error && entry != fs::recursive_directory_iterator{}; entry.increment(error)) {
        const bool isDirectory = entry->is_directory(error);
        if (error) break;
        if (!isDirectory) continue;
        const auto path = FileSystem::PathToUtf8(entry->path());
        if (IsVisibleAssetBrowserFolder(path)) {
            /// @note ジャンクションやシンボリックリンクから別の枝や祖先へ再帰しない。
            const auto canonical = fs::canonical(entry->path(), error);
            const auto expected = folder.canonicalPath / entry->path().lexically_relative(folder.path);
            if (!error && FileSystem::SamePathText(FileSystem::PathToUtf8(canonical),
                                                  FileSystem::PathToUtf8(expected))) {
                keys.push_back(AssetBrowserFolderKey(path));
                continue;
            }
        }
        entry.disable_recursion_pending();
        if (error) break;
    }
    return keys;
}

bool RememberFolderColor(EditorContext& ctx, std::uint32_t color)
{
    auto& recent = ctx.assetBrowserRecentFolderColors;
    if (!recent.empty() && recent.front() == color) return false;
    std::erase(recent, color);
    recent.insert(recent.begin(), color);
    constexpr std::size_t MAX_RECENT_COLORS = 8;
    if (recent.size() > MAX_RECENT_COLORS) recent.resize(MAX_RECENT_COLORS);
    return true;
}

OpResult UpdateFolderColor(EditorContext& ctx, const ColorFolder& folder,
                           std::optional<std::uint32_t> color, bool recursive)
{
    std::error_code error;
    const auto keys = CollectColorFolders(folder, recursive, error);
    if (error) return OpResult::Err("FOLDER_READ_FAILED", "サブフォルダを列挙できませんでした。");

    auto& colors = ctx.assetBrowserFolderColors;
    int changedCount = 0;
    for (const auto& key : keys) {
        if (color) {
            const auto found = colors.find(key);
            if (found != colors.end() && found->second == *color) continue;
            colors.insert_or_assign(key, *color);
            ++changedCount;
        } else if (colors.erase(key) != 0) {
            ++changedCount;
        }
    }
    const bool recentChanged = color && RememberFolderColor(ctx, *color);
    const bool changed = changedCount > 0 || recentChanged;
    if (changed) ctx.requestEditorSettingsSave = true;
    auto data = OpData::MakeObject();
    data.Set("path", OpData(FileSystem::PathToUtf8(folder.path)));
    data.Set("color", color ? OpData(FormatAssetBrowserFolderColor(*color)) : OpData{});
    data.Set("changedCount", OpData(changedCount));
    auto result = OpResult::Data(std::move(data));
    result.noChange = !changed;
    return result;
}

void RegisterFolderColorOperators(OperatorRegistry& registry)
{
    for (const std::string_view action : {"inspect", "set", "clear"}) {
        const bool inspect = action == "inspect";
        const bool set = action == "set";
        EditorOperator op;
        op.id = "asset.folder_color." + std::string(action);
        op.label = inspect ? "Inspect Folder Color" : set ? "Set Folder Color" : "Clear Folder Color";
        op.category = "AssetBrowser";
        op.kind = inspect ? OpKind::Query : OpKind::Action;
        op.desc = inspect ? "フォルダの色を #RRGGBB または未設定 (null) で返す。"
                         : "Asset Browser のフォルダ色設定を変更して保存する。";
        op.params = { AssetPathParam("Assets/ 以下のプロジェクト相対パス、または Asset Browser 内の絶対パス") };
        if (set)
            op.params.push_back({"color", OpParamType::String, "不透明な RGB 色 (#RRGGBB)", true});
        if (!inspect)
            op.params.push_back({"recursive", OpParamType::Bool, "表示対象のサブフォルダにも適用する", false, false});
        op.poll = [set](const OpContext& context, const OpArgs& args) {
            if (args.Empty()) return !context.ctx.projectRoot.empty();
            return (!set || ParseFolderColor(args.GetString("color")).has_value()) &&
                ResolveColorFolder(context.ctx, args.GetString("path")).has_value();
        };
        op.exec = [inspect, set](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            const auto folder = ResolveColorFolder(ctx, args.GetString("path"));
            if (!folder) return OpResult::Err("BAD_PATH", "Asset Browser に表示できるフォルダを指定してください。");
            if (!inspect) {
                const auto color = set ? ParseFolderColor(args.GetString("color")) : std::nullopt;
                if (set && !color) return OpResult::Err("BAD_COLOR", "#RRGGBB で色を指定してください。");
                return UpdateFolderColor(ctx, *folder, color, args.GetBool("recursive"));
            }
            const auto path = FileSystem::PathToUtf8(folder->path);
            const auto found = ctx.assetBrowserFolderColors.find(AssetBrowserFolderKey(path));
            OpData data = OpData::MakeObject();
            data.Set("path", OpData(path));
            data.Set("color", found != ctx.assetBrowserFolderColors.end()
                ? OpData(FormatAssetBrowserFolderColor(found->second)) : OpData{});
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}

/// @brief プロジェクト直下に C# Script 用プロジェクトが一つだけあるか確認する。
/// @param projectRoot ゲームプロジェクトのルート。
/// @return `<Project>.Scripts.csproj` が一つだけなら true。
bool HasSingleManagedProject(const std::string& projectRoot)
{
    std::error_code error;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator iterator(projectRoot, error);
         !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        const std::filesystem::path path = iterator->path();
        if (path.extension() == ".csproj" && path.stem().wstring().ends_with(L".Scripts"))
            ++count;
    }
    return !error && count == 1;
}

} /// @note namespace

void RegisterAssetBrowserOperators(OperatorRegistry& registry)
{
    RegisterFolderColorOperators(registry);
    RegisterManagedDataAssetOperators(registry);
    RegisterModelImportOperators(registry);
    RegisterTextureImportOperators(registry);
    RegisterAnimationPreviewOperators(registry);
    RegisterAssetFileOperators(registry);
    RegisterAssetReferenceOperators(registry);
    RegisterProjectSettingsOperators(registry);
    RegisterInputBindingOperators(registry);
    const auto registerCSharpCreation = [&registry](const char* id,
                                                     const char* label,
                                                     const char* description,
                                                     ScriptCodeGen::CSharpKind kind) {
        EditorOperator op;
        op.id = id;
        op.label = label;
        op.category = "AssetBrowser";
        op.desc = description;
        op.kind = OpKind::Action;
        OpParam name;
        name.name = "name";
        name.type = OpParamType::String;
        name.desc = "生成する C# 型の ASCII 識別子";
        name.required = true;
        op.params = { std::move(name) };
        op.poll = [](const OpContext& context, const OpArgs& args) {
            return !context.ctx.projectRoot.empty() &&
                   ScriptCodeGen::IsValidCSharpName(args.GetString("name"));
        };
        op.exec = [kind](OpContext& context, const OpArgs& args) {
            const std::string name = args.GetString("name");
            if (!HasSingleManagedProject(context.ctx.projectRoot))
                return OpResult::Err("CSHARP_PROJECT_SETUP",
                    "Exactly one <Project>.Scripts.csproj is required; use the SDK ManagedStarter to set it up.");
            const std::string scriptsDir = context.ctx.projectRoot + "/Assets/Scripts";
            const std::string path = ScriptCodeGen::CreateCSharpClass(name, scriptsDir, kind);
            if (path.empty())
                return OpResult::Err("CREATE_FAILED", "C# source could not be created; the name may already exist.");

            context.ctx.requestAssetBrowserRefresh = true;
            context.ctx.requestRevealAssetPath = path;
            OpData data = OpData::MakeObject();
            data.Set("path", OpData(path));
            const std::string className = kind == ScriptCodeGen::CSharpKind::Script && !name.ends_with("Component")
                ? name + "Component" : name;
            data.Set("className", OpData(className));
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    };

    registerCSharpCreation("asset.create_csharp_script", "Create C# Script",
        "Assets/Scripts に protected override lifecycle を持つ C# Script を作成する。",
        ScriptCodeGen::CSharpKind::Script);
    registerCSharpCreation("asset.create_csharp_helper", "Create C# Helper",
        "Assets/Scripts に GameObject へ追加しない C# helper 型を作成する。",
        ScriptCodeGen::CSharpKind::Helper);
    registerCSharpCreation("asset.create_csharp_data_asset_type", "Create C# DataAsset Type",
        "Assets/Scripts に共有値を定義する C# DataAsset 型を作成する。",
        ScriptCodeGen::CSharpKind::DataAsset);

    {
        EditorOperator op;
        op.id       = "asset.refresh";
        op.label    = "Refresh Asset Browser";
        op.category = "AssetBrowser";
        op.desc     = "AssetBrowser の一覧とファイル監視由来の検索結果を更新する。";
        op.kind     = OpKind::Action;
        op.exec     = [](OpContext& context, const OpArgs&) {
            context.ctx.requestAssetBrowserRefresh = true;
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "asset.reveal";
        op.label    = "Reveal Asset in Browser";
        op.category = "AssetBrowser";
        op.desc     = "指定アセットを AssetBrowser で選択し、参照先へ移動する。";
        op.kind     = OpKind::Action;
        op.params   = { AssetPathParam("絶対パスまたは Assets 起点のアセットパス") };

        OpParam select;
        select.name         = "select";
        select.type         = OpParamType::Bool;
        select.desc         = "true の場合は Inspector の表示対象もアセットへ移す";
        select.required     = false;
        select.defaultValue = false;
        op.params.push_back(std::move(select));
        op.poll = [](const OpContext&, const OpArgs& args) {
            return !args.GetString("path").empty();
        };
        op.exec = [](OpContext& context, const OpArgs& args) {
            context.ctx.requestRevealAssetPath   = args.GetString("path");
            context.ctx.requestRevealAssetSelect = args.GetBool("select");
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }

}

} /// @note namespace fbzz::editor
