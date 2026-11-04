/// @file    HistoryOperators.cpp
/// @brief   共通Undo履歴の照会・移動・消去を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
namespace fbzz::editor {
void RegisterHistoryOperators(OperatorRegistry& registry)
{
    for (const bool control : {false, true}) {
        EditorOperator op; op.id = control ? "edit.history.control" : "edit.history.inspect"; op.label = op.id; op.category = "Edit";
        op.kind = control ? OpKind::Action : OpKind::Query;
        op.desc = "Undo Historyと同じ履歴を扱う。controlのjumpはindexへ移動し、競合した操作で止まる。clearは履歴を消去する。revision指定で照会後の変更を検出できる。";
        if (control) {
            OpParam p; p.name = "action"; p.enumValues = {"jump", "clear"}; op.params.push_back(p);
            p = {}; p.name = "index"; p.type = OpParamType::Int; p.required = false; op.params.push_back(p);
            p = {}; p.name = "revision"; p.type = OpParamType::Int; p.required = false; op.params.push_back(p);
        }
        op.exec = [control](OpContext& context, const OpArgs& args) {
            auto& history = context.undo;
            if (control) {
                if (args.Has("revision") && (args.GetInt("revision", -1) < 0 || static_cast<std::size_t>(args.GetInt("revision")) != history.GetRevision())) return OpResult::Err("STALE_HISTORY", "履歴が変更されました。再照会してください。");
                if (args.GetString("action") == "clear") history.Clear();
                else {
                    const int index = args.GetInt("index", -1); if (index < 0 || static_cast<std::size_t>(index) > history.GetHistorySize()) return OpResult::Err("BAD_ARG", "indexが履歴の範囲外です。");
                    history.JumpTo(static_cast<std::size_t>(index));
                    if (history.GetCursor() != static_cast<std::size_t>(index)) return OpResult::Err("HISTORY_CONFLICT", history.LastDiagnostic());
                }
            }
            OpData data = OpData::MakeObject(), entries = OpData::MakeArray();
            for (const auto& entry : history.GetHistory()) { OpData row = OpData::MakeObject(); row.Set("description", OpData(entry.description)); row.Set("applied", OpData(entry.applied)); entries.Push(std::move(row)); }
            data.Set("entries", std::move(entries)); data.Set("cursor", OpData(static_cast<std::int64_t>(history.GetCursor()))); data.Set("revision", OpData(static_cast<std::int64_t>(history.GetRevision())));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
