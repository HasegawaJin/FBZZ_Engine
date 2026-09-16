/// @file    AssetBrowserOperators.cpp
/// @brief   AssetBrowser のナビゲーションとインポート入口を Operator として公開する。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY: AssetBrowser の一覧・検査・サムネイルは既に AI Query から利用できるが、
/// 「この参照先を人間の画面でも確認する」「再インポート設定を開く」といった
/// 検証導線は UI だけに閉じていた。EditorContext の one-shot request を共有し、
/// パネルの所有権やファイル監視の実装を Operator 層へ漏らさない。
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>

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

} // namespace

void RegisterAssetBrowserOperators(OperatorRegistry& registry)
{
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

} // namespace fbzz::editor
