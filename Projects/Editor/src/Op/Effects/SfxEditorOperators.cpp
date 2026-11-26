/// @file    SfxEditorOperators.cpp
/// @brief   SFX Editorの表示設定を共通操作へ登録する。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#include <Editor/Panels/Tools/SfxEditorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>

namespace fbzz::editor {
void SfxEditorPanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const bool configure : {false, true}) {
        EditorOperator op;
        op.id = configure ? "sfx.editor.configure" : "sfx.editor.inspect";
        op.label = op.id;
        op.category = "SFX";
        op.kind = configure ? OpKind::Action : OpKind::Query;
        op.dialog = OpDialog::NONE;
        op.desc = "SFX EditorのUI編集時の自動試聴を照会・変更し、個人設定へ保存する。";
        if (configure) {
            op.params.push_back({"autoPreview", OpParamType::Bool, "UI編集時の自動試聴", false, {}});
        }
        op.exec = [this, configure](OpContext& context, const OpArgs& args) {
            if (configure && args.Has("autoPreview")) {
                m_autoPreview = args.GetBool("autoPreview");
                context.ctx.requestEditorSettingsSave = true;
            }
            auto data = OpData::MakeObject();
            data.Set("autoPreview", m_autoPreview);
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}
} // namespace fbzz::editor
