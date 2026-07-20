// FBZZ Engine
// EditorBusProtocol.hpp | fbzz::editor::ai
// Editor Command Bus の wire 契約 (envelope) を C++ 側にミラーする。
//
// TypeScript 側 (Projects/EditorMcp/src/editorContracts.ts) と一字一句そろえることが唯一の真実。
// 要求は NDJSON の EditorBusRequest {protocol,id,kind,payload,dryRun,source}、
// 応答は EditorBusResponse {protocol,id,ok,result?,error?}。payload の判別子は "t"。
#pragma once
#include <Editor/Ai/Json.hpp>
#include <optional>
#include <string>

namespace fbzz::editor::ai {

// MCP 側の EDITOR_PROTOCOL と一致させる。非互換世代の要求を弾く単一の識別子。
inline constexpr const char* kEditorProtocol = "fbzz.editor.v1";

// 受信した1件の要求エンベロープ。source は現状 "mcp" のみで処理には使わない。
struct BusRequest {
    std::string id;             // 応答の相関キー (MCP が発行した UUID)
    std::string kind;           // "query" | "command"
    JsonValue   payload;        // "t" を持つ Query/Command 本体
    bool        dryRun = false; // true の Command は実変更せず試算のみ

    bool        IsQuery()   const { return kind == "query"; }
    bool        IsCommand() const { return kind == "command"; }

    // payload["t"] を返す (無ければ空文字)。ディスパッチの分岐キー。
    std::string PayloadType() const
    {
        const JsonValue* t = payload.Find("t");
        return (t != nullptr && t->IsString()) ? t->AsString() : std::string{};
    }
};

// パース済み JSON エンベロープを検証して BusRequest へ変換する。
// protocol 不一致・必須欠落は nullopt を返し error に理由を書く。id が取れないと相関不能なため必須。
std::optional<BusRequest> ParseBusRequest(const JsonValue& root, std::string* error);

// 成功応答 {protocol,id,ok:true,result} を組み立てる。
JsonValue MakeOkResponse(const std::string& id, JsonValue result);

// 失敗応答 {protocol,id,ok:false,error:{code,message}} を組み立てる。throw の代替として使う。
JsonValue MakeErrorResponse(const std::string& id, const std::string& code, const std::string& message);

} // namespace fbzz::editor::ai
