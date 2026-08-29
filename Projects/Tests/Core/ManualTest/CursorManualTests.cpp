/// @file    CursorManualTests.cpp
/// @brief   OS カーソルの表示・拘束・Editor 復元を開発者が目視確認する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <gtest/gtest.h>

#include <Engine/Core/Cursor.hpp>

namespace fbzz::tests {

TEST(CursorManualTest, AppliesAndRestoresCursorState)
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

} // namespace fbzz::tests
