/// @file    AssetBrowserOperators.cpp
/// @brief   AssetBrowser のナビゲーションとインポート入口を Operator として公開する。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// AssetBrowser の一覧・検査・サムネイルは AI Query から既に使えるが、参照確認や再インポート設定を
/// 開く検証導線は UI だけに閉じていた。EditorContext の one-shot request を共有し、パネルの所有権や
/// ファイル監視の実装を Operator 層へ漏らさない。
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Editor/Util/ManagedDataAssetEditor.hpp>

#include <Editor/EditorContext.hpp>

#include <filesystem>
#include <system_error>
#include <utility>

namespace fbzz::editor {

namespace {

OpParam AssetPathParam(const char* description)
{
    OpParam param;
    param.name     = "path";
    param.type     = OpParamType::String;
    param.desc     = description;
    param.required = true;
    return param;
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
    RegisterManagedDataAssetOperators(registry);
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

    {
        EditorOperator op;
        op.id       = "asset.open_import_settings";
        op.label    = "Open Asset Import Settings";
        op.category = "AssetBrowser";
        op.desc     = "指定アセットの既存インポート設定モーダルを開く。";
        op.kind     = OpKind::Action;
        op.params   = { AssetPathParam("再インポートするアセットのパス") };
        op.poll = [](const OpContext&, const OpArgs& args) {
            return !args.GetString("path").empty();
        };
        op.exec = [](OpContext& context, const OpArgs& args) {
            context.ctx.requestOpenImportModal = args.GetString("path");
            return OpResult::Ok();
        };
        registry.Register(std::move(op));
    }
}

} /// @note namespace fbzz::editor
