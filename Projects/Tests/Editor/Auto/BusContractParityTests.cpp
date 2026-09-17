/// @file    BusContractParityTests.cpp
/// @brief   MCP 側の契約 (editorContracts.ts) と Editor 側のハンドラー表の型名が一致することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @note    片側だけに型を足すと «MCP のツールは有るのに UNKNOWN_COMMAND» か «Editor に実装が有るのに AI から呼べない» になる。どちらも実行するまで気付けない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Ai/EditorBusDispatcher.hpp>

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

class BusContractParityTest : public testkit::EditorFixture {};

} // namespace

TEST_F(BusContractParityTest, EveryContractTypeHasAHandler)
{
    const editor::ai::EditorBusDispatcher dispatcher(Context());
    const std::vector<std::string> registered = dispatcher.RegisteredTypes();
    const std::set<std::string> contract = ContractTypes();
    ASSERT_GT(contract.size(), 100u) << "契約ファイルの読み取りに失敗している";

    for (const std::string& type : contract) {
        EXPECT_NE(std::find(registered.begin(), registered.end(), type), registered.end())
            << type << " は editorContracts.ts に有るが Editor のハンドラー表に無い";
    }
}

TEST_F(BusContractParityTest, EveryHandlerIsReachableFromTheContract)
{
    const editor::ai::EditorBusDispatcher dispatcher(Context());
    const std::set<std::string> contract = ContractTypes();

    for (const std::string& type : dispatcher.RegisteredTypes()) {
        EXPECT_TRUE(contract.count(type) > 0) << type << " は Editor に実装が有るが editorContracts.ts に無い (AI から呼べない)";
    }
}

TEST_F(BusContractParityTest, NoTypeIsRegisteredTwice)
{
    const editor::ai::EditorBusDispatcher dispatcher(Context());
    std::vector<std::string> registered = dispatcher.RegisteredTypes();
    std::sort(registered.begin(), registered.end());
    EXPECT_EQ(std::adjacent_find(registered.begin(), registered.end()), registered.end());
}

} // namespace fbzz::tests
