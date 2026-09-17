/// @file    EditorBusProtocol.cpp
/// @brief   wire エンベロープの検証と応答生成。
/// @author  Hasegawa Jin
/// @date    2026-07-20
#include <Editor/Ai/EditorBusProtocol.hpp>

namespace fbzz::editor::ai {

std::optional<BusRequest> ParseBusRequest(const JsonValue& root, std::string* error)
{
    const auto fail = [&](const char* message) -> std::optional<BusRequest> {
        if (error != nullptr) *error = message;
        return std::nullopt;
    };

    if (!root.IsObject()) return fail("要求がオブジェクトではありません");

    const JsonValue* protocol = root.Find("protocol");
    if (protocol == nullptr || !protocol->IsString() || protocol->AsString() != kEditorProtocol) {
        return fail("protocol が一致しません");
    }

    const JsonValue* id = root.Find("id");
    if (id == nullptr || !id->IsString() || id->AsString().empty()) {
        return fail("id が必要です");
    }

    const JsonValue* kind = root.Find("kind");
    if (kind == nullptr || !kind->IsString()) return fail("kind が必要です");
    const std::string kindValue = kind->AsString();
    if (kindValue != "query" && kindValue != "command") return fail("kind は query か command です");

    const JsonValue* payload = root.Find("payload");
    if (payload == nullptr || !payload->IsObject()) return fail("payload が必要です");
    const JsonValue* payloadType = payload->Find("t");
    if (payloadType == nullptr || !payloadType->IsString()) return fail("payload.t が必要です");

    BusRequest request;
    request.id      = id->AsString();
    request.kind    = kindValue;
    request.payload = *payload;
    /// @note dryRun は Command のみ意味を持つ。欠落や非 bool は安全側 (試算) の true にする。
    const JsonValue* dryRun = root.Find("dryRun");
    request.dryRun = (dryRun != nullptr && dryRun->IsBool()) ? dryRun->AsBool() : true;
    return request;
}

JsonValue MakeOkResponse(const std::string& id, JsonValue result)
{
    JsonValue response = JsonValue::MakeObject();
    response.Set("protocol", JsonValue(kEditorProtocol));
    response.Set("id", JsonValue(id));
    response.Set("ok", JsonValue(true));
    response.Set("result", std::move(result));
    return response;
}

JsonValue MakeErrorResponse(const std::string& id, const std::string& code, const std::string& message)
{
    JsonValue error = JsonValue::MakeObject();
    error.Set("code", JsonValue(code));
    error.Set("message", JsonValue(message));

    JsonValue response = JsonValue::MakeObject();
    response.Set("protocol", JsonValue(kEditorProtocol));
    response.Set("id", JsonValue(id));
    response.Set("ok", JsonValue(false));
    response.Set("error", std::move(error));
    return response;
}

} // namespace fbzz::editor::ai
