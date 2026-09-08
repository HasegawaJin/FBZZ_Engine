/// @file    CursorManualTests.cpp
/// @brief   OS カーソルの表示・拘束・要求スタック・Editor 復元を開発者が目視確認する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>

#include <Engine/Core/Cursor.hpp>

namespace fbzz::tests {

class CursorManualTest : public testkit::Fixture {
protected:
    // 静的状態を持つ API なので、前のテストの要求を持ち越さない。
    void SetUp() override { core::Cursor::ResetForEditor(); }
    void TearDown() override { core::Cursor::ResetForEditor(); }
};

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

TEST_F(CursorManualTest, ShapeNamesSurviveTomlRoundTrip)
{
    for (std::size_t i = 0; i < core::kCursorShapeCount; ++i) {
        const auto shape = static_cast<core::CursorShape>(i);
        EXPECT_EQ(core::CursorShapeFromString(core::ToString(shape)), shape);
    }
    EXPECT_EQ(core::CursorShapeFromString("bogus"), core::CursorShape::Default);
}

TEST_F(CursorManualTest, SuppressionKeepsTheGameRequest)
{
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

TEST_F(CursorManualTest, StrongerRequestWinsAndPoppingRestoresTheWeakerOne)
{
    // 視点操作 (Camera) の上へメニュー (Modal) を重ね、閉じたら視点へ «自動で» 戻る。
    // これが SetLockMode の直書きでは書けない部分で、戻す値を覚える責任が消える。
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

TEST_F(CursorManualTest, RequestsAreListedStrongestFirst)
{
    core::Cursor::Push({ core::CursorLockMode::Locked, false },
                       core::CursorPriority::Camera, 1, "Camera");
    core::Cursor::Push({ core::CursorLockMode::Confined, true },
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

TEST_F(CursorManualTest, OwnerReleaseDropsEveryRequestFromThatObject)
{
    core::Cursor::Push({ core::CursorLockMode::Locked, false },
                       core::CursorPriority::Camera, 7, "Camera");
    core::Cursor::Push({ core::CursorLockMode::Confined, true },
                       core::CursorPriority::UI, 7, "CameraUi");
    core::Cursor::Push({ core::CursorLockMode::Confined, false },
                       core::CursorPriority::UI, 9, "Other");

    core::Cursor::ReleaseByOwner(7);
    EXPECT_EQ(core::Cursor::GetRequestCount(), 1u);
    EXPECT_FALSE(core::Cursor::IsVisible());
}

TEST_F(CursorManualTest, ClearRequestsAlsoResetsTheBase)
{
    // シーン遷移で呼ぶ経路。積んだ要求だけでなく «直書きの基底» まで戻さないと、
    // 前の画面が SetLockMode で取った拘束を誰も外さない。
    core::Cursor::SetLockMode(core::CursorLockMode::Locked);
    core::Cursor::Push({ core::CursorLockMode::Confined, false },
                       core::CursorPriority::UI, 1, "Screen");

    core::Cursor::ClearRequests();
    EXPECT_EQ(core::Cursor::GetRequestCount(), 0u);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::None);
    EXPECT_TRUE(core::Cursor::IsVisible());
}

TEST_F(CursorManualTest, UpdatingARequestKeepsItsPlaceInTheStack)
{
    const auto ui = core::Cursor::Push({ core::CursorLockMode::Confined, false },
                                       core::CursorPriority::UI, 1, "Pointer");
    core::Cursor::Push({ core::CursorLockMode::Locked, false },
                       core::CursorPriority::Camera, 2, "Camera");

    // マウスとパッドで表示だけを切り替える経路。Release → Push だと «同値なら後勝ち» の
    // 順序が変わり、他の要求との強弱が黙って入れ替わる。
    core::Cursor::UpdateRequest(ui, { core::CursorLockMode::Confined, true });
    EXPECT_TRUE(core::Cursor::IsVisible());
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Confined);

    core::CursorRequestInfo top{};
    ASSERT_TRUE(core::Cursor::GetRequest(0, top));
    EXPECT_STREQ(top.label, "Pointer");
}

} // namespace fbzz::tests
