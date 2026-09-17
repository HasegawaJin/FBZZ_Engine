/// @file    EditorBusDispatcher.cpp
/// @brief   Editor Command Bus のメインスレッド処理。要求を型名の表で領域ごとのハンドラーへ振り分ける。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Editor/Ai/EditorBusDispatcher.hpp>

#include "Bus/BusInternal.hpp"

#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/UndoStack.hpp>

#include <memory>
#include <optional>
#include <string>

namespace fbzz::editor::ai {

namespace {

using bus::BusCall;
using bus::BusHandlerEntry;
using bus::BusKind;
using bus::Outcome;

/// @brief 複数の Undo 可能 Command を 1 つの Undo 単位にまとめて適用する (editor.transaction)。
/// @note 要素は BUILDER か、transaction 用 builder を併せ持つ COMMAND だけ。焼き等の取り消せない操作は混ぜさせない。
Outcome DoTransaction(BusCall& call)
{
    editor::EditorContext& ctx = call.ctx;
    if (ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
    const std::string label = bus::StringField(call.payload, "label");
    const JsonValue* cmds = call.payload.Find("cmds");
    if (cmds == nullptr || !cmds->IsArray() || cmds->AsArray().empty()) {
        return Outcome::Err("BAD_ARG", "cmds 配列が必要です");
    }
    auto composite = std::make_unique<CompositeCommand>(label.empty() ? "AI: Transaction" : label);
    for (const JsonValue& sub : cmds->AsArray()) {
        if (!sub.IsObject()) return Outcome::Err("BAD_ARG", "cmds の要素がオブジェクトではありません");
        const std::string subType = bus::StringField(sub, "t");
        const BusHandlerEntry* entry = call.table.Find(subType);
        if (entry == nullptr || entry->builder == nullptr) {
            if (subType.starts_with("fluid.")) {
                return Outcome::Err("UNSUPPORTED",
                    "この fluid コマンドは transaction 内で使用できません (焼き・プレビューは Undo できないため単独で呼んでください): " + subType);
            }
            return Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + subType);
        }
        Outcome subErr;
        std::unique_ptr<ICommand> subCommand = entry->builder(ctx, subType, sub, subErr, nullptr, nullptr);
        if (subCommand == nullptr) return subErr;
        composite->Add(std::move(subCommand));
    }
    if (call.dryRun) {
        JsonValue result = JsonValue::MakeObject();
        result.Set("dryRun", JsonValue(true));
        result.Set("would", JsonValue("editor.transaction"));
        result.Set("count", JsonValue(static_cast<int>(cmds->AsArray().size())));
        return Outcome::Ok(std::move(result));
    }
    ctx.undoStack->Execute(std::move(composite));
    JsonValue result = JsonValue::MakeObject();
    result.Set("committed", JsonValue(label));
    result.Set("count", JsonValue(static_cast<int>(cmds->AsArray().size())));
    return Outcome::Ok(std::move(result));
}

/// @brief 登録済みの型名と種別を返す (editor.bus.list)。MCP 契約との突き合わせに使う。
Outcome DoBusList(BusCall& call)
{
    JsonValue types = JsonValue::MakeArray();
    for (const BusHandlerEntry& entry : call.table.Entries()) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("t", JsonValue(entry.type));
        item.Set("kind", JsonValue(entry.kind == BusKind::QUERY ? "query" : entry.kind == BusKind::COMMAND ? "command" : "undoable"));
        item.Set("transaction", JsonValue(entry.builder != nullptr));
        types.Push(std::move(item));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("types", std::move(types));
    return Outcome::Ok(std::move(result));
}

/// @brief BUILDER の型を組み立て、dryRun なら予告だけ返し、そうでなければ UndoStack へ積む。
Outcome RunBuilder(BusCall& call, const BusHandlerEntry& entry)
{
    auto createdSink = entry.returnsCreatedId ? std::make_shared<std::string>() : nullptr;
    JsonValue detail = JsonValue::MakeObject();
    Outcome err;
    std::unique_ptr<ICommand> command = entry.builder(call.ctx, call.type, call.payload, err, createdSink, &detail);
    if (command == nullptr) return err;
    /// @note dryRun でも副作用の予告は返す。適用前に改名や上限の引き上げを知れる方が意味がある。
    if (call.dryRun) {
        Outcome preview = bus::DryRunPreview(call.type);
        if (preview.ok && !detail.AsObject().empty()) preview.result.Set("detail", std::move(detail));
        return preview;
    }
    if (call.ctx.undoStack == nullptr) return Outcome::Err("NO_UNDOSTACK", "UndoStack が未設定です");
    call.ctx.undoStack->Execute(std::move(command));
    JsonValue result = JsonValue::MakeObject();
    result.Set("applied", JsonValue(true));
    result.Set("t", JsonValue(call.type));
    if (createdSink && !createdSink->empty()) result.Set("id", JsonValue(*createdSink));
    if (!detail.AsObject().empty()) result.Set("detail", std::move(detail));
    return Outcome::Ok(std::move(result));
}

} // namespace

EditorBusDispatcher::EditorBusDispatcher(editor::EditorContext& context)
    : m_context(context)
    , m_table(std::make_unique<bus::BusHandlerTable>())
    , m_state(std::make_unique<bus::BusState>())
{
    bus::RegisterEditorHandlers(*m_table);
    bus::RegisterSceneHandlers(*m_table);
    bus::RegisterAssetHandlers(*m_table);
    bus::RegisterBehaviorTreeHandlers(*m_table);
    bus::RegisterPlayHandlers(*m_table);
    bus::RegisterAnimationHandlers(*m_table);
    bus::RegisterWorldHandlers(*m_table);
    bus::RegisterFluidHandlers(*m_table);
    bus::RegisterPlaytestHandlers(*m_table);
    m_table->AddCommand("editor.transaction", DoTransaction);
    m_table->AddQuery("editor.bus.list", DoBusList);
}

EditorBusDispatcher::~EditorBusDispatcher() = default;

void EditorBusDispatcher::SetSceneViewportRT(renderer::ResourceHandle<renderer::RenderTargetTag> rt)
{
    m_state->sceneViewportRT = rt;
}

void EditorBusDispatcher::SetGameViewportRT(renderer::ResourceHandle<renderer::RenderTargetTag> rt)
{
    m_state->gameViewportRT = rt;
}

std::vector<std::string> EditorBusDispatcher::RegisteredTypes() const
{
    std::vector<std::string> types;
    types.reserve(m_table->Entries().size());
    for (const BusHandlerEntry& entry : m_table->Entries()) types.push_back(entry.type);
    return types;
}

std::string EditorBusDispatcher::Handle(const std::string& requestLine)
{
    /// @note 空行は NDJSON の区切りとして正常。要求ではないので黙って捨てる。
    if (requestLine.find_first_not_of(" \t\r\n") == std::string::npos) return {};

    std::string parseError;
    std::optional<JsonValue> root = ParseJson(requestLine, &parseError);
    if (!root.has_value()) {
        /// @note id を取り出せなくても応答は返す。黙ると送信側は «届いていない» と «壊れていた» を区別できず timeout まで固まる。
        return SerializeJson(MakeErrorResponse({}, "BAD_JSON", parseError));
    }

    std::optional<BusRequest> request = ParseBusRequest(*root, &parseError);
    if (!request.has_value()) {
        const JsonValue* id = root->Find("id");
        const std::string correlationId =
            (id != nullptr && id->IsString()) ? id->AsString() : std::string{};
        return SerializeJson(MakeErrorResponse(correlationId, "BAD_REQUEST", parseError));
    }

    const std::string type = request->PayloadType();
    BusCall call{ m_context, type, request->payload, request->dryRun, *m_state, *m_table };
    const BusHandlerEntry* entry = m_table->Find(type);

    Outcome outcome;
    if (request->IsQuery()) {
        outcome = (entry != nullptr && entry->kind == BusKind::QUERY)
            ? entry->handler(call)
            : Outcome::Err("UNKNOWN_QUERY", "未対応の Query: " + type);
    } else if (entry == nullptr || entry->kind == BusKind::QUERY) {
        outcome = Outcome::Err("UNKNOWN_COMMAND", "未対応の Command: " + type);
    } else if (entry->kind == BusKind::COMMAND) {
        outcome = entry->handler(call);
    } else {
        outcome = RunBuilder(call, *entry);
    }

    if (outcome.ok) return SerializeJson(MakeOkResponse(request->id, std::move(outcome.result)));
    return SerializeJson(MakeErrorResponse(request->id, outcome.code, outcome.message));
}

} // namespace fbzz::editor::ai
