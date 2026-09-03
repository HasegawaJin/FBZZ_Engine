/// @file    CursorManualTests.cpp
/// @brief   OS カーソルの表示・拘束・Editor 復元を開発者が目視確認する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>

#include <Engine/Core/Cursor.hpp>

namespace fbzz::tests {

class CursorManualTest : public testkit::Fixture {};

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

TEST_F(CursorManualTest, LockModeSurvivesTomlRoundTrip)
{
    for (core::CursorLockMode mode : { core::CursorLockMode::None,
                                       core::CursorLockMode::Confined,
                                       core::CursorLockMode::Locked }) {
        EXPECT_EQ(core::CursorLockModeFromString(core::ToString(mode)), mode);
    }
    // 未知の綴りは既定 (拘束しない) へ倒す。設定ファイルの手編集でカーソルを
    // 取り上げられたまま起動する事故を作らない。
    EXPECT_EQ(core::CursorLockModeFromString("bogus"), core::CursorLockMode::None);
}

TEST_F(CursorManualTest, SuppressionKeepsTheGameRequest)
{
    core::Cursor::ResetForEditor();

    // 抑制中でも «ゲームが何を求めたか» は残る。Editor が Game View から離れている間に
    // 要求を None へ畳んでしまうと、戻ってきたときに何を復元するか分からなくなる。
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

TEST_F(CursorManualTest, PolicyKnowsWhenItTakesTheCursor)
{
    EXPECT_FALSE((core::CursorPolicy{ core::CursorLockMode::None, true }).CapturesCursor());
    EXPECT_TRUE((core::CursorPolicy{ core::CursorLockMode::None, false }).CapturesCursor());
    EXPECT_TRUE((core::CursorPolicy{ core::CursorLockMode::Confined, true }).CapturesCursor());
}

} // namespace fbzz::tests
