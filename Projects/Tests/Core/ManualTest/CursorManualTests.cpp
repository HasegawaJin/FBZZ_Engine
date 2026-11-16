/// @file    CursorManualTests.cpp
/// @brief   OS カーソルの表示・拘束・要求スタック・Editor 復元を開発者が目視確認する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>

#include <Engine/Core/Cursor.hpp>

#include <tuple>

namespace fbzz::tests {

/// @note 個別に解放しない要求は一括操作か TearDown で破棄するため、要求 ID の破棄を明示する。
/// @see https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/c4834 MSVC の nodiscard 診断と std::ignore。
class CursorManualTest : public testkit::Fixture {
protected:
    /// @note Cursor の静的状態をリセットし、テスト間で要求が残らないようにする。
    void SetUp() override { core::Cursor::ResetForEditor(); }
    void TearDown() override { core::Cursor::ResetForEditor(); }
};

/// @brief Applies and restores cursor state.
TEST_F(CursorManualTest, AppliesAndRestoresCursorState)
{
    core::Cursor::SetVisible(true);
    core::Cursor::SetLockMode(core::CursorLockMode::None);
    core::Cursor::ApplyLock();
    EXPECT_TRUE(core::Cursor::IsVisible());
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::None);

    core::Cursor::SetLockMode(core::CursorLockMode::Locked);
    core::Cursor::ApplyLock();
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Locked);

    core::Cursor::ResetForEditor();
    EXPECT_TRUE(core::Cursor::IsVisible());
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::None);
}

/// @brief Lock mode survives TOML round trip.
TEST_F(CursorManualTest, LockModeSurvivesTomlRoundTrip)
{
    for (core::CursorLockMode mode : { core::CursorLockMode::None,
                                       core::CursorLockMode::Confined,
                                       core::CursorLockMode::Locked }) {
        EXPECT_EQ(core::CursorLockModeFromString(core::ToString(mode)), mode);
    }
    /// @note 設定ファイルに未知の綴りがあっても、起動時はカーソルを拘束しない。
    EXPECT_EQ(core::CursorLockModeFromString("bogus"), core::CursorLockMode::None);
}

/// @brief Shape names survive TOML round trip.
TEST_F(CursorManualTest, ShapeNamesSurviveTomlRoundTrip)
{
    for (std::size_t i = 0; i < core::kCursorShapeCount; ++i) {
        const auto shape = static_cast<core::CursorShape>(i);
        EXPECT_EQ(core::CursorShapeFromString(core::ToString(shape)), shape);
    }
    EXPECT_EQ(core::CursorShapeFromString("bogus"), core::CursorShape::Default);
}

/// @brief Suppression keeps the game request.
TEST_F(CursorManualTest, SuppressionKeepsTheGameRequest)
{
    /// @note Game View に戻ったときに復元できるよう、OS への反映を抑制している間もゲームの要求を保持する。
    core::Cursor::SetSuppressed(true);
    core::Cursor::SetLockMode(core::CursorLockMode::Locked);
    core::Cursor::SetVisible(false);
    EXPECT_TRUE(core::Cursor::IsSuppressed());
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Locked);
    EXPECT_FALSE(core::Cursor::IsVisible());

    core::Cursor::ResetForEditor();
    EXPECT_FALSE(core::Cursor::IsSuppressed());
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::None);
    EXPECT_TRUE(core::Cursor::IsVisible());
}

/// @brief Policy knows when it takes the cursor.
TEST_F(CursorManualTest, PolicyKnowsWhenItTakesTheCursor)
{
    EXPECT_FALSE((core::CursorPolicy{ core::CursorLockMode::None, true }).CapturesCursor());
    EXPECT_TRUE((core::CursorPolicy{ core::CursorLockMode::None, false }).CapturesCursor());
    EXPECT_TRUE((core::CursorPolicy{ core::CursorLockMode::Confined, true }).CapturesCursor());
}

/// @brief Stronger request wins and popping restores the weaker one.
TEST_F(CursorManualTest, StrongerRequestWinsAndPoppingRestoresTheWeakerOne)
{
    /// @note メニューを閉じると、視点操作が要求した拘束と表示がスタックから復元される。
    const auto camera = core::Cursor::Push({ core::CursorLockMode::Locked, false },
                                           core::CursorPriority::Camera, 1, "Camera");
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Locked);

    const auto menu = core::Cursor::Push({ core::CursorLockMode::Confined, true },
                                         core::CursorPriority::Modal, 2, "Menu");
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Confined);
    EXPECT_TRUE(core::Cursor::IsVisible());

    core::Cursor::Release(menu);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Locked);
    EXPECT_FALSE(core::Cursor::IsVisible());

    core::Cursor::Release(camera);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::None);
    EXPECT_TRUE(core::Cursor::IsVisible());
}

/// @brief Same priority lets the latest request win.
TEST_F(CursorManualTest, SamePriorityLetsTheLatestRequestWin)
{
    const auto first = core::Cursor::Push({ core::CursorLockMode::Confined, true },
                                          core::CursorPriority::UI, 1, "First");
    const auto second = core::Cursor::Push({ core::CursorLockMode::Locked, false },
                                           core::CursorPriority::UI, 2, "Second");
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Locked);

    core::Cursor::Release(second);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Confined);
    core::Cursor::Release(first);
}

/// @brief Requests are listed strongest first.
TEST_F(CursorManualTest, RequestsAreListedStrongestFirst)
{
    std::ignore = core::Cursor::Push({ core::CursorLockMode::Locked, false },
                                    core::CursorPriority::Camera, 1, "Camera");
    std::ignore = core::Cursor::Push({ core::CursorLockMode::Confined, true },
                                    core::CursorPriority::Modal, 2, "Menu");

    ASSERT_EQ(core::Cursor::GetRequestCount(), 2u);
    core::CursorRequestInfo top{};
    ASSERT_TRUE(core::Cursor::GetRequest(0, top));
    EXPECT_STREQ(top.label, "Menu");
    EXPECT_TRUE(top.active);

    core::CursorRequestInfo below{};
    ASSERT_TRUE(core::Cursor::GetRequest(1, below));
    EXPECT_STREQ(below.label, "Camera");
    EXPECT_FALSE(below.active);
}

/// @brief Owner release drops every request from that object.
TEST_F(CursorManualTest, OwnerReleaseDropsEveryRequestFromThatObject)
{
    std::ignore = core::Cursor::Push({ core::CursorLockMode::Locked, false },
                                    core::CursorPriority::Camera, 7, "Camera");
    std::ignore = core::Cursor::Push({ core::CursorLockMode::Confined, true },
                                    core::CursorPriority::UI, 7, "CameraUi");
    std::ignore = core::Cursor::Push({ core::CursorLockMode::Confined, false },
                                    core::CursorPriority::UI, 9, "Other");

    core::Cursor::ReleaseByOwner(7);
    EXPECT_EQ(core::Cursor::GetRequestCount(), 1u);
    EXPECT_FALSE(core::Cursor::IsVisible());
}

/// @brief Clear requests also resets the base.
TEST_F(CursorManualTest, ClearRequestsAlsoResetsTheBase)
{
    /// @note シーン遷移時に基底もリセットし、前のシーンが直接指定した拘束を解除する。
    core::Cursor::SetLockMode(core::CursorLockMode::Locked);
    std::ignore = core::Cursor::Push({ core::CursorLockMode::Confined, false },
                                    core::CursorPriority::UI, 1, "Screen");

    core::Cursor::ClearRequests();
    EXPECT_EQ(core::Cursor::GetRequestCount(), 0u);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::None);
    EXPECT_TRUE(core::Cursor::IsVisible());
}

/// @brief Updating a request keeps its place in the stack.
TEST_F(CursorManualTest, UpdatingARequestKeepsItsPlaceInTheStack)
{
    const auto ui = core::Cursor::Push({ core::CursorLockMode::Confined, false },
                                       core::CursorPriority::UI, 1, "Pointer");
    std::ignore = core::Cursor::Push({ core::CursorLockMode::Locked, false },
                                    core::CursorPriority::Camera, 2, "Camera");

    /// @note マウスとパッドで表示だけを切り替えるときも、同じ優先度での登録順を保持する。
    core::Cursor::UpdateRequest(ui, { core::CursorLockMode::Confined, true });
    EXPECT_TRUE(core::Cursor::IsVisible());
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Confined);

    core::CursorRequestInfo top{};
    ASSERT_TRUE(core::Cursor::GetRequest(0, top));
    EXPECT_STREQ(top.label, "Pointer");
}

} // namespace fbzz::tests
