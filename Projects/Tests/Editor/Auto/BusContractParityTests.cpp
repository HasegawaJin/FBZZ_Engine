/// @file    BusContractParityTests.cpp
/// @brief   MCP 側の契約 (editorContracts.ts) と editor.bus.list 応答の型名が一致することを検証する。
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
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

/// @brief 契約ファイルの型名。TypeScript の型 (`t: 'x'`) と zod (`z.literal('x')` を持つ t) の両方から拾う。
std::set<std::string> ContractTypes()
{
    const std::filesystem::path file = std::filesystem::path(FBZZ_SOURCE_DIR) / "Projects/EditorMcp/src/editorContracts.ts";
    std::ifstream stream(file, std::ios::binary);
    EXPECT_TRUE(static_cast<bool>(stream)) << file.generic_string();
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    std::set<std::string> types;
    const std::regex pattern(R"(\bt:\s*(?:z\.literal\()?'([A-Za-z][A-Za-z0-9_.]*)')");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it)
        types.insert((*it)[1].str());
    return types;
}

class BusContractParityTest : public testkit::EditorFixture {
protected:
    void QueryBusTypes(std::vector<std::string>& types)
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
            types.push_back(type->AsString());
        }
    }
};

} /// @note namespace

TEST_F(BusContractParityTest, EveryContractTypeHasAHandler)
{
    std::vector<std::string> registered;
    ASSERT_NO_FATAL_FAILURE(QueryBusTypes(registered));
    const std::set<std::string> contract = ContractTypes();
    ASSERT_GT(contract.size(), 100u) << "契約ファイルの読み取りに失敗している";

    for (const std::string& type : contract) {
        EXPECT_NE(std::find(registered.begin(), registered.end(), type), registered.end())
            << type << " は editorContracts.ts に有るが Editor のハンドラー表に無い";
    }
}

TEST_F(BusContractParityTest, EveryHandlerIsReachableFromTheContract)
{
    std::vector<std::string> registered;
    ASSERT_NO_FATAL_FAILURE(QueryBusTypes(registered));
    const std::set<std::string> contract = ContractTypes();
    ASSERT_GT(contract.size(), 100u) << "契約ファイルの読み取りに失敗している";

    for (const std::string& type : registered) {
        EXPECT_TRUE(contract.count(type) > 0) << type << " は Editor に実装が有るが editorContracts.ts に無い (AI から呼べない)";
    }
}

TEST_F(BusContractParityTest, NoTypeIsRegisteredTwice)
{
    std::vector<std::string> registered;
    ASSERT_NO_FATAL_FAILURE(QueryBusTypes(registered));
    std::sort(registered.begin(), registered.end());
    EXPECT_EQ(std::adjacent_find(registered.begin(), registered.end()), registered.end());
}

} /// @note namespace fbzz::tests
