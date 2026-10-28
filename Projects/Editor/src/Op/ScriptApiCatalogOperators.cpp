/// @file    ScriptApiCatalogOperators.cpp
/// @brief   UI と AI に同じ SDK metadata の読み取りを提供する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Editor/Op/ScriptApiCatalog.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Util/FileSystem.hpp>

namespace fbzz::editor {
void RegisterScriptApiCatalogOperator(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "script.api.catalog"; op.label = "Script SDK API"; op.category = "Script";
    op.desc = "公開済み C# SDK の署名・nullability・単位・座標・callback・失敗契約を読み取る";
    op.kind = OpKind::Query;
    op.exec = [](OpContext&, const OpArgs&) {
        scene::ManagedScriptRuntime& runtime = scene::ManagedScriptRuntime::Instance();
        std::string text;
        if (!runtime.GetApiCatalog(text)) {
            /// @note C# 型をまだ作っていない native-only project でも SDK の補完情報は取得できる。
            if (!runtime.Initialize(util::FileSystem::GetExecutableDirectory() / L"Managed") || !runtime.GetApiCatalog(text))
                return OpResult::Err("SDK_API_UNAVAILABLE", runtime.LastDiagnostic());
        }
        OpData result = OpData::MakeObject(); result.Set("format", "json"); result.Set("catalogJson", std::move(text));
        return OpResult::Data(std::move(result));
    };
    registry.Register(std::move(op));
}
} /// @note namespace fbzz::editor
