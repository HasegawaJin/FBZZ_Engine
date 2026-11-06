/// @file    BusContractParityTests.cpp
/// @brief   MCP と C++ の操作名・種別・プロトコルと共有入力ケースの整合を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @note    片側だけに型を足すと «MCP のツールは有るのに UNKNOWN_COMMAND» か «Editor に実装が有るのに AI から呼べない» になる。どちらも実行するまで気付けない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::ai::JsonValue;
using ContractMap = std::map<std::string, std::string>;

std::string ReadSource(const char* relativePath)
{
    const std::filesystem::path file = std::filesystem::path(FBZZ_SOURCE_DIR) / relativePath;
    std::ifstream stream(file, std::ios::binary);
    EXPECT_TRUE(static_cast<bool>(stream)) << file.generic_string();
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// @note 型宣言と実行時スキーマを混ぜると、一方から消えた操作をもう一方が隠してしまう。
void AppendContractTypes(const std::string& source, std::string_view beginMarker,
                         std::string_view endMarker, const char* kind,
                         const std::regex& pattern, ContractMap& types)
{
    const auto begin = source.find(beginMarker);
    ASSERT_NE(begin, std::string::npos) << beginMarker;
    const auto end = source.find(endMarker, begin + beginMarker.size());
    ASSERT_NE(end, std::string::npos) << endMarker;
    const std::string section = source.substr(begin, end - begin);
    std::size_t count = 0;
    for (auto it = std::sregex_iterator(section.begin(), section.end(), pattern); it != std::sregex_iterator(); ++it) {
        const std::string type = (*it)[1].str();
        EXPECT_TRUE(types.emplace(type, kind).second) << kind << ": duplicate contract " << type;
        ++count;
    }
    EXPECT_GT(count, 0u) << beginMarker;
}

ContractMap ContractTypes(bool runtimeSchema = true)
{
    const std::string source = ReadSource("Projects/EditorMcp/src/editorContracts.ts");
    const std::regex pattern(runtimeSchema
        ? R"(\bt:\s*z\.literal\('([A-Za-z][A-Za-z0-9_.]*)')"
        : R"(\bt:\s*'([A-Za-z][A-Za-z0-9_.]*)')");
    ContractMap types;
    AppendContractTypes(source,
        runtimeSchema ? "export const EditorQuerySchema" : "export type EditorQuery =",
        runtimeSchema ? "const CommandNameSchema" : "export const EditorQuerySchema",
        "query", pattern, types);
    AppendContractTypes(source,
        runtimeSchema ? "export const EditorCommandSchema" : "export type EditorCommand =",
        runtimeSchema ? "export interface EditorBusRequest" : "export const EditorCommandSchema",
        "command", pattern, types);
    return types;
}

struct BusType {
    std::string type;
    std::string kind;
};

class BusContractParityTest : public testkit::EditorFixture {
protected:
    void QueryBusTypes(std::vector<BusType>& types)
    {
        using editor::ai::JsonValue;
        JsonValue payload = JsonValue::MakeObject();
        payload.Set("t", JsonValue("editor.bus.list"));
        JsonValue request = JsonValue::MakeObject();
        request.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", JsonValue("bus-contract-parity"));
        request.Set("kind", JsonValue("query"));
        request.Set("payload", payload);
        request.Set("dryRun", JsonValue(false));
        editor::ai::EditorBusDispatcher dispatcher(Context());
        const std::string responseText = dispatcher.Handle(editor::ai::SerializeJson(request));
        std::string parseError;
        const auto response = editor::ai::ParseJson(responseText, &parseError);
        ASSERT_TRUE(response.has_value()) << parseError << "\n" << responseText;
        ASSERT_TRUE(response->IsObject()) << responseText;
        const JsonValue* protocol = response->Find("protocol");
        ASSERT_NE(protocol, nullptr) << responseText;
        ASSERT_TRUE(protocol->IsString()) << responseText;
        ASSERT_EQ(protocol->AsString(), editor::ai::kEditorProtocol);
        const JsonValue* id = response->Find("id");
        ASSERT_NE(id, nullptr) << responseText;
        ASSERT_TRUE(id->IsString()) << responseText;
        ASSERT_EQ(id->AsString(), "bus-contract-parity");
        const JsonValue* ok = response->Find("ok");
        ASSERT_NE(ok, nullptr) << responseText;
        ASSERT_TRUE(ok->IsBool()) << responseText;
        ASSERT_TRUE(ok->AsBool()) << responseText;
        const JsonValue* result = response->Find("result");
        ASSERT_NE(result, nullptr) << responseText;
        ASSERT_TRUE(result->IsObject()) << responseText;
        const JsonValue* entries = result->Find("types");
        ASSERT_NE(entries, nullptr) << responseText;
        ASSERT_TRUE(entries->IsArray()) << responseText;
        ASSERT_FALSE(entries->AsArray().empty()) << responseText;

        types.clear();
        for (const JsonValue& entry : entries->AsArray()) {
            ASSERT_TRUE(entry.IsObject()) << responseText;
            const JsonValue* type = entry.Find("t");
            ASSERT_NE(type, nullptr) << responseText;
            ASSERT_TRUE(type->IsString()) << responseText;
            ASSERT_FALSE(type->AsString().empty()) << responseText;
            const JsonValue* kind = entry.Find("kind");
            ASSERT_NE(kind, nullptr) << responseText;
            ASSERT_TRUE(kind->IsString()) << responseText;
            ASSERT_TRUE(kind->AsString() == "query" || kind->AsString() == "command" || kind->AsString() == "undoable") << responseText;
            /// @note undoable は native 内部の区分で、wire では command として送る。
            types.push_back({type->AsString(), kind->AsString() == "query" ? "query" : "command"});
        }
    }
};

} /// @note namespace

TEST_F(BusContractParityTest, EveryContractTypeHasAHandler)
{
    std::vector<BusType> registered;
    ASSERT_NO_FATAL_FAILURE(QueryBusTypes(registered));
    const ContractMap contract = ContractTypes();
    ASSERT_GT(contract.size(), 100u) << "契約ファイルの読み取りに失敗している";

    for (const auto& [type, kind] : contract) {
        EXPECT_NE(std::find_if(registered.begin(), registered.end(), [&](const BusType& entry) { return entry.type == type; }), registered.end())
            << type << " は editorContracts.ts に有るが Editor のハンドラー表に無い";
    }
}

TEST_F(BusContractParityTest, EveryHandlerIsReachableFromTheContract)
{
    std::vector<BusType> registered;
    ASSERT_NO_FATAL_FAILURE(QueryBusTypes(registered));
    const ContractMap contract = ContractTypes();
    ASSERT_GT(contract.size(), 100u) << "契約ファイルの読み取りに失敗している";

    for (const BusType& entry : registered) {
        EXPECT_TRUE(contract.count(entry.type) > 0) << entry.type << " は Editor に実装が有るが editorContracts.ts に無い (AI から呼べない)";
    }
}

TEST_F(BusContractParityTest, NoTypeIsRegisteredTwice)
{
    std::vector<BusType> registered;
    ASSERT_NO_FATAL_FAILURE(QueryBusTypes(registered));
    ContractMap types;
    for (const BusType& entry : registered) EXPECT_TRUE(types.emplace(entry.type, entry.kind).second) << entry.type;
}

TEST_F(BusContractParityTest, QueryAndCommandKindsMatchNativeHandlers)
{
    std::vector<BusType> registered;
    ASSERT_NO_FATAL_FAILURE(QueryBusTypes(registered));
    const ContractMap contract = ContractTypes();
    ASSERT_GT(contract.size(), 100u);

    for (const BusType& entry : registered) {
        const auto found = contract.find(entry.type);
        ASSERT_NE(found, contract.end()) << entry.type;
        EXPECT_EQ(found->second, entry.kind) << entry.type;
    }
}

TEST_F(BusContractParityTest, TypeAliasesAndRuntimeSchemasDeclareTheSameKinds)
{
    const ContractMap types = ContractTypes(false);
    const ContractMap schemas = ContractTypes();
    ASSERT_GT(types.size(), 100u);
    EXPECT_EQ(types, schemas);
}

TEST_F(BusContractParityTest, ProtocolMatchesTypeScriptAndSharedCases)
{
    const std::string source = ReadSource("Projects/EditorMcp/src/editorContracts.ts");
    const std::regex pattern(R"(export const EDITOR_PROTOCOL\s*=\s*'([^']+)')");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(source, match, pattern));
    EXPECT_EQ(match[1].str(), editor::ai::kEditorProtocol);
    const auto fixture = editor::ai::ParseJson(ReadSource("Projects/EditorMcp/src/tests/fixtures/EditorContractCases.json"));
    ASSERT_TRUE(fixture.has_value());
    const JsonValue* protocol = fixture->Find("protocol");
    ASSERT_NE(protocol, nullptr);
    ASSERT_TRUE(protocol->IsString());
    EXPECT_EQ(protocol->AsString(), editor::ai::kEditorProtocol);
}

TEST_F(BusContractParityTest, SharedInputCasesMatchNativeDryRun)
{
    const auto fixture = editor::ai::ParseJson(ReadSource("Projects/EditorMcp/src/tests/fixtures/EditorContractCases.json"));
    ASSERT_TRUE(fixture.has_value());
    const JsonValue* cases = fixture->Find("cases");
    const JsonValue* nodeId = fixture->Find("nodeId");
    ASSERT_NE(cases, nullptr);
    ASSERT_TRUE(cases->IsArray());
    ASSERT_FALSE(cases->AsArray().empty());
    ASSERT_NE(nodeId, nullptr);
    ASSERT_TRUE(nodeId->IsString());
    auto& node = AttachScene().CreateGameObject("Contract node");
    node.instanceId = nodeId->AsString();
    editor::ai::EditorBusDispatcher dispatcher(Context());

    for (const JsonValue& entry : cases->AsArray()) {
        const JsonValue* name = entry.Find("name");
        const JsonValue* kind = entry.Find("kind");
        const JsonValue* payload = entry.Find("payload");
        const JsonValue* accepted = entry.Find("accepted");
        ASSERT_NE(name, nullptr);
        ASSERT_TRUE(name->IsString());
        SCOPED_TRACE(name->AsString());
        ASSERT_NE(kind, nullptr);
        ASSERT_TRUE(kind->IsString());
        ASSERT_TRUE(kind->AsString() == "query" || kind->AsString() == "command");
        ASSERT_NE(payload, nullptr);
        ASSERT_TRUE(payload->IsObject());
        ASSERT_NE(accepted, nullptr);
        ASSERT_TRUE(accepted->IsBool());
        JsonValue request = JsonValue::MakeObject();
        request.Set("protocol", JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", JsonValue(name->AsString()));
        request.Set("kind", *kind);
        request.Set("payload", *payload);
        request.Set("dryRun", JsonValue(true));
        const std::string responseText = dispatcher.Handle(editor::ai::SerializeJson(request));
        const auto response = editor::ai::ParseJson(responseText);
        ASSERT_TRUE(response.has_value()) << responseText;
        const JsonValue* ok = response->Find("ok");
        ASSERT_NE(ok, nullptr);
        ASSERT_TRUE(ok->IsBool());
        EXPECT_EQ(ok->AsBool(), accepted->AsBool()) << responseText;
        if (!accepted->AsBool()) {
            const JsonValue* error = response->Find("error");
            ASSERT_NE(error, nullptr) << responseText;
            const JsonValue* code = error->Find("code");
            ASSERT_NE(code, nullptr) << responseText;
            EXPECT_EQ(code->AsString(), "BAD_ARG") << responseText;
        }
    }
    EXPECT_EQ(node.layer, 0);
    EXPECT_TRUE(node.activeSelf());
    EXPECT_NE(node.tag, "Enemy");
    EXPECT_VEC3_NEAR(node.transform.position, math::Vector3::ZERO, 0.000001f);
}

} /// @note namespace fbzz::tests
