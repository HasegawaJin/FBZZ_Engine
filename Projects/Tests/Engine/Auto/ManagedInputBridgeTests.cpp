/// @file    ManagedInputBridgeTests.cpp
/// @brief   登録済み無入力と未知名を区別し、入力名 metadata が実行中の map に追従することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedInputBridge.hpp"
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

namespace fbzz::tests {

class ManagedInputBridgeTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        input::InputActionMap::Clear();
        input::InputActionMap::SetEnabled(true);
    }
    void TearDown() override
    {
        input::InputActionMap::Clear();
        input::InputActionMap::SetEnabled(true);
        EngineFixture::TearDown();
    }
    void AddButton(const std::string& name)
    {
        input::InputAction action;
        action.name = name;
        ASSERT_TRUE(input::InputActionMap::AddAction(action));
    }
    void AddAxis(const std::string& name)
    {
        input::InputAxis axis;
        axis.name = name;
        ASSERT_TRUE(input::InputActionMap::AddAxis(axis));
    }
    void ExpectNames(std::string_view bytes, std::initializer_list<std::string_view> expected)
    {
        uint32_t count = 0;
        ASSERT_GE(bytes.size(), sizeof(count));
        std::memcpy(&count, bytes.data(), sizeof(count));
        EXPECT_EQ(count, expected.size());
        bytes.remove_prefix(sizeof(count));
        for (std::string_view name : expected) {
            uint32_t length = 0;
            ASSERT_GE(bytes.size(), sizeof(length));
            std::memcpy(&length, bytes.data(), sizeof(length));
            bytes.remove_prefix(sizeof(length));
            ASSERT_GE(bytes.size(), length);
            EXPECT_EQ(bytes.substr(0, length), name);
            bytes.remove_prefix(length);
        }
        EXPECT_TRUE(bytes.empty());
    }
    using Bridge = scene::managed_detail::ManagedInputBridge;
    using Status = scene::ManagedBridgeStatus;
    using Query = scene::ManagedInputQuery;
    using Value = scene::ManagedInputValue;
};

TEST_F(ManagedInputBridgeTest, DistinguishesRegisteredZeroValuesFromUnknownNamesAndKinds)
{
    AddButton("Fire");
    AddAxis("MoveX");
    input::InputActionMap::Update(0.0f);
    scene::ManagedBridgeReply reply;
    for (Value state : {Value::HELD, Value::DOWN, Value::UP}) {
        ASSERT_EQ(Bridge::Read("Fire", static_cast<uint32_t>(state), reply), Status::OK);
        EXPECT_EQ(reply.valueKind, scene::ManagedValueKind::BOOL);
        EXPECT_EQ(reply.integer, 0);
    }
    ASSERT_EQ(Bridge::Read("MoveX", static_cast<uint32_t>(Value::AXIS), reply), Status::OK);
    EXPECT_EQ(reply.valueKind, scene::ManagedValueKind::FLOAT);
    EXPECT_FLOAT_EQ(reply.x, 0.0f);

    reply.x = 17.0f;
    EXPECT_EQ(Bridge::Read("fire", static_cast<uint32_t>(Value::HELD), reply), Status::NOT_FOUND);
    EXPECT_EQ(Bridge::Read("Fire", static_cast<uint32_t>(Value::AXIS), reply), Status::NOT_FOUND);
    EXPECT_EQ(Bridge::Read("MoveX", static_cast<uint32_t>(Value::HELD), reply), Status::NOT_FOUND);
    EXPECT_EQ(Bridge::Read("Horizontal", static_cast<uint32_t>(Value::AXIS), reply), Status::NOT_FOUND);
    EXPECT_FLOAT_EQ(reply.x, 17.0f);
    /// @note C++ の既存の未知名に対する false / 0 の契約は変更しない。
    EXPECT_FALSE(input::InputActionMap::GetAction("fire"));
    EXPECT_FLOAT_EQ(input::InputActionMap::GetAxis("Horizontal"), 0.0f);
}

TEST_F(ManagedInputBridgeTest, MetadataTracksCurrentMapAndPreservesUtf8RegistrationOrder)
{
    AddButton("Fire");
    AddButton("ジャンプ");
    AddAxis("MoveX");
    scene::ManagedBridgeReply reply;
    std::string names;
    ASSERT_EQ(Bridge::Query("", static_cast<uint32_t>(Query::BUTTON_NAMES), reply, names), Status::OK);
    EXPECT_EQ(reply.valueKind, scene::ManagedValueKind::STRING_ARRAY);
    EXPECT_EQ(reply.index, 2);
    ExpectNames(names, {"Fire", "ジャンプ"});
    ASSERT_EQ(Bridge::Query("MoveX", static_cast<uint32_t>(Query::BUTTON_EXISTS), reply, names), Status::OK);
    EXPECT_EQ(reply.integer, 0);
    ASSERT_EQ(Bridge::Query("MoveX", static_cast<uint32_t>(Query::AXIS_EXISTS), reply, names), Status::OK);
    EXPECT_EQ(reply.integer, 1);
    ASSERT_TRUE(input::InputActionMap::RemoveAction("Fire"));
    AddButton("Alternate");
    ASSERT_EQ(Bridge::Query("Fire", static_cast<uint32_t>(Query::BUTTON_EXISTS), reply, names), Status::OK);
    EXPECT_EQ(reply.integer, 0);
    EXPECT_EQ(Bridge::Read("Fire", static_cast<uint32_t>(Value::HELD), reply), Status::NOT_FOUND);
    ASSERT_EQ(Bridge::Query("", static_cast<uint32_t>(Query::BUTTON_NAMES), reply, names), Status::OK);
    ExpectNames(names, {"ジャンプ", "Alternate"});
    input::InputActionMap::SetEnabled(false);
    ASSERT_EQ(Bridge::Query("MoveX", static_cast<uint32_t>(Query::AXIS_EXISTS), reply, names), Status::OK);
    EXPECT_EQ(reply.integer, 1);
    ASSERT_EQ(Bridge::Read("MoveX", static_cast<uint32_t>(Value::AXIS), reply), Status::OK);
    EXPECT_FLOAT_EQ(reply.x, 0.0f);
}

TEST_F(ManagedInputBridgeTest, InvalidQueriesLeaveOutputsUnchanged)
{
    scene::ManagedBridgeReply reply;
    reply.index = 27;
    std::string names = "sentinel";
    EXPECT_EQ(Bridge::Query("", static_cast<uint32_t>(Query::BUTTON_EXISTS), reply, names), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Bridge::Query("Fire", static_cast<uint32_t>(Query::BUTTON_NAMES), reply, names), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Bridge::Query("Fire", 99, reply, names), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Bridge::Read("", static_cast<uint32_t>(Value::HELD), reply), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Bridge::Read("Fire", 99, reply), Status::INVALID_ARGUMENT);
    EXPECT_EQ(reply.index, 27);
    EXPECT_EQ(names, "sentinel");
}

TEST_F(ManagedInputBridgeTest, CatalogRetainsAllNamesBeyondFormerTotalAndSingleNameLimits)
{
    const std::string giant(1024 * 1024 + 23, 'x'); AddButton("First"); AddButton(giant); AddButton("Last");
    scene::ManagedBridgeReply reply; std::string names;
    ASSERT_EQ(Bridge::Query("", static_cast<uint32_t>(Query::BUTTON_NAMES), reply, names), Status::OK);
    EXPECT_GT(names.size(), 1024U * 1024U); EXPECT_EQ(reply.index, 3U); ExpectNames(names, {"First", giant, "Last"});
}

} /// @note namespace fbzz::tests
