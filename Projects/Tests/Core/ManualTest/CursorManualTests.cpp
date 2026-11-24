/// @file    CursorManualTests.cpp
/// @brief   OS カーソルの表示・拘束・要求スタック・Editor 復元を開発者が目視確認する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <array>
#include <bit>
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

/// @brief カーソルの表示と拘束を適用し、エディターへ戻す。
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

/// @brief 拘束モードを設定文字列と相互変換する。
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

/// @brief 形状名を設定文字列と相互変換する。
TEST_F(CursorManualTest, ShapeNamesSurviveTomlRoundTrip)
{
    for (std::size_t i = 0; i < core::kCursorShapeCount; ++i) {
        const auto shape = static_cast<core::CursorShape>(i);
        EXPECT_EQ(core::CursorShapeFromString(core::ToString(shape)), shape);
    }
    EXPECT_EQ(core::CursorShapeFromString("bogus"), core::CursorShape::Default);
}

/// @brief 入力の抑制中もゲームの要求を保持する。
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

/// @brief カーソルを引き受けるポリシーを判定する。
TEST_F(CursorManualTest, PolicyKnowsWhenItTakesTheCursor)
{
    EXPECT_FALSE((core::CursorPolicy{ core::CursorLockMode::None, true }).CapturesCursor());
    EXPECT_TRUE((core::CursorPolicy{ core::CursorLockMode::None, false }).CapturesCursor());
    EXPECT_TRUE((core::CursorPolicy{ core::CursorLockMode::Confined, true }).CapturesCursor());
}

/// @brief 強い要求の解除後に弱い要求を復元する。
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

/// @brief 同じ優先度では後から追加した要求を採用する。
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

/// @brief 要求一覧を有効な順に取得する。
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

/// @brief 同じ所有者の要求をまとめて解除する。
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

/// @brief 要求の一括解除で基底の設定も復元する。
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

/// @brief 要求を更新しても優先順位を維持する。
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

TEST_F(CursorManualTest, CustomShapePreservesWindowClassAndFallbackCursor)
{
    auto& application = core::Application::Get();
    struct Shutdown {
        core::Application& application;
        bool physicalInputAllowed;
        ~Shutdown()
        {
            application.Shutdown();
            core::Cursor::SetPhysicalInputAllowed(physicalInputAllowed);
        }
    } shutdown{application, core::Cursor::IsPhysicalInputAllowed()};

    core::Cursor::SetPhysicalInputAllowed(false);
    ASSERT_TRUE(application.Init({L"Cursor shape test", 320, 240, false}));
    core::Cursor::SetPhysicalInputAllowed(true);

    const HWND window = application.GetWindow().GetHandle();
    const HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    const auto classCursor = GetClassLongPtrW(window, GCLP_HCURSOR);
    ASSERT_NE(classCursor, 0u);
    const auto setCursorMessage = [&](WORD hit) {
        return SendMessageW(window, WM_SETCURSOR, std::bit_cast<WPARAM>(window), MAKELPARAM(hit, WM_MOUSEMOVE));
    };
    setCursorMessage(HTLEFT);
    const HCURSOR resizeCursor = GetCursor();

    testkit::TempDir temp("cursor-shape");
    ASSERT_TRUE(temp.IsValid());
    const auto path = temp.File("cursor.tga");
    /// @note 2 × 2 の非圧縮 BGRA 画像。先頭画素のアルファは 128。
    const std::array<unsigned char, 34> tga{
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28,
        16, 32, 64, 128, 32, 64, 128, 255, 0, 0, 0, 0, 0, 0, 0, 255
    };
    ASSERT_TRUE(util::FileSystem::WriteBinary(path, tga.data(), tga.size()));

    core::Cursor::SetWindowActive(true);
    ASSERT_TRUE(core::Cursor::AcquireGameInput());
    ASSERT_TRUE(core::Cursor::SetShapeImage(core::CursorShape::Default, path.string().c_str(), 1, 0));
    EXPECT_EQ(GetClassLongPtrW(window, GCLP_HCURSOR), classCursor);

    EXPECT_EQ(setCursorMessage(HTCLIENT), TRUE);
    const HCURSOR custom = GetCursor();
    ASSERT_NE(custom, nullptr);
    ASSERT_NE(custom, arrow);

    struct CursorBitmaps {
        ICONINFO info{};
        ~CursorBitmaps()
        {
            if (info.hbmColor) DeleteObject(info.hbmColor);
            if (info.hbmMask) DeleteObject(info.hbmMask);
        }
    } cursorBitmaps;
    ASSERT_TRUE(GetIconInfo(custom, &cursorBitmaps.info));
    EXPECT_FALSE(cursorBitmaps.info.fIcon);
    EXPECT_EQ(cursorBitmaps.info.xHotspot, 1u);
    EXPECT_EQ(cursorBitmaps.info.yHotspot, 0u);

    BITMAPINFO bmp{};
    bmp.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmp.bmiHeader.biWidth = 2;
    bmp.bmiHeader.biHeight = -2;
    bmp.bmiHeader.biPlanes = 1;
    bmp.bmiHeader.biBitCount = 32;
    bmp.bmiHeader.biCompression = BI_RGB;
    std::array<unsigned char, 16> pixels{};
    HDC dc = GetDC(nullptr);
    ASSERT_NE(dc, nullptr);
    const int rows = GetDIBits(dc, cursorBitmaps.info.hbmColor, 0, 2, pixels.data(), &bmp, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    ASSERT_EQ(rows, 2);
    EXPECT_EQ(pixels[3], 128);
    EXPECT_EQ(pixels[7], 255);

    const HCURSOR textCursor = LoadCursor(nullptr, IDC_IBEAM);
    application.GetWindow().SetWndProcHook([textCursor](HWND, UINT msg, WPARAM, LPARAM lp) {
        if (msg != WM_SETCURSOR || LOWORD(lp) != HTCLIENT) return false;
        SetCursor(textCursor);
        return true;
    });
    EXPECT_EQ(setCursorMessage(HTCLIENT), TRUE);
    EXPECT_EQ(GetCursor(), textCursor);
    application.GetWindow().SetWndProcHook({});

    setCursorMessage(HTLEFT);
    EXPECT_EQ(GetCursor(), resizeCursor);
    core::Cursor::ReleaseGameInput();
    setCursorMessage(HTCLIENT);
    EXPECT_EQ(GetCursor(), arrow);
}

}
