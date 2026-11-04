/// @file    ConsoleOperators.cpp
/// @brief   コンソールの表示行・選択・消去を直接操作する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <algorithm>
namespace fbzz::editor {
void ConsolePanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "clear", "select"}) {
        EditorOperator op; const std::string name = action;
        op.id = "console." + name; op.label = op.id; op.category = "Console";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "現在の検索・レベル・Collapseを反映した行を返す。設定はworkspace.settings.configure。selectは通し番号の配列で選択し、selectedTextでコピー内容を得る。clearは保持ログを消去する。";
        if (name == "inspect") {
            for (const char* key : {"offset", "limit"}) { OpParam p; p.name = key; p.type = OpParamType::Int; p.required = false; op.params.push_back(p); }
        }
        if (name == "select") { OpParam p; p.name = "sequences"; p.type = OpParamType::Data; op.params.push_back(p); }
        op.exec = [this, name](OpContext&, const OpArgs& args) {
            RebuildRows();
            if (name == "clear") { m_sink.Clear(); m_selection.clear(); m_anchorSequence = 0; m_detailSequence = 0; RebuildRows(); }
            if (name == "select") {
                const auto* sequences = args.GetData("sequences");
                if (!sequences || !sequences->IsArray() || sequences->AsArray().size() > ConsoleSink::MAX_ENTRIES) return OpResult::Err("BAD_ARG", "sequencesは表示行の通し番号の配列です。");
                std::unordered_set<std::uint64_t> next; std::uint64_t detail = 0;
                for (const auto& value : sequences->AsArray()) {
                    if (!value.IsInt() || value.AsInt64() <= 0 || RowIndexOf(static_cast<std::uint64_t>(value.AsInt64())) < 0) return OpResult::Err("STALE_ROW", "表示中の行が見つかりません。");
                    detail = static_cast<std::uint64_t>(value.AsInt64()); next.insert(detail);
                }
                m_selection = std::move(next); m_detailSequence = detail; m_anchorSequence = detail;
            }
            const int offset = args.GetInt("offset", 0), limit = args.GetInt("limit", 100);
            if (offset < 0 || limit < 1 || limit > 512) return OpResult::Err("BAD_ARG", "offset>=0、limit=1..512です。");
            OpData data = OpData::MakeObject(), rows = OpData::MakeArray(); const auto& entries = m_sink.GetEntries();
            const auto end = (std::min)(m_rows.size(), static_cast<std::size_t>(offset) + static_cast<std::size_t>(limit));
            for (auto i = static_cast<std::size_t>(offset); i < end; ++i) {
                const auto& row = m_rows[i]; if (row.entryIndex >= entries.size()) continue;
                OpData item = OpData::MakeObject(); item.Set("sequence", OpData(static_cast<std::int64_t>(row.sequence)));
                item.Set("level", OpData(static_cast<int>(row.level))); item.Set("count", OpData(row.count));
                item.Set("message", OpData(entries[row.entryIndex].message)); item.Set("selected", OpData(m_selection.contains(row.sequence))); rows.Push(std::move(item));
            }
            data.Set("rows", std::move(rows)); data.Set("total", OpData(static_cast<std::int64_t>(m_rows.size())));
            data.Set("selectedText", OpData(SelectedText())); data.Set("revision", OpData(static_cast<std::int64_t>(m_sink.GetRevision())));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
