/// @file    Main.cpp
/// @brief   全テスト exe の共通エントリポイント。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// gtest_main / gmock_main を直接使わず自前で持つ理由は 3 つ。
///   - InitGoogleMock を確実に通す (gtest_main だけでは gmock が初期化されない)
///   - 結果が読める状態で画面に残るようコンソールを整える
///   - «テスト全体に一度だけ» 効かせたい設定を足す場所を確保しておく
#include <TestKit/Console.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>

int main(int argc, char** argv)
{
    /// @note InitGoogleMock より前に判定する。Init は認識したフラグを argv から取り除くため、後で見ると --gtest_list_tests が消えていて列挙中に入力待ちへ入る。
    const bool automated = fbzz::testkit::IsAutomatedRun(argc, argv);

    bool ownsConsole = false;
    if (automated) {
        /// @note 人が居ない実行では «押されるまで消えないダイアログ» が即ハングになる。
        ///       1 件の assert / クラッシュで残り全部が走らなくなるのを防ぐ。
        fbzz::testkit::SuppressBlockingErrorDialogs();
    } else {
        ownsConsole = fbzz::testkit::EnsureConsole();
    }

    /// @note InitGoogleMock は内部で InitGoogleTest も呼ぶ。--gtest_* / --gmock_* の両方が効く。
    ::testing::InitGoogleMock(&argc, argv);

    /// @note EXPECT_CALL を書いていないメソッドが呼ばれたら «警告» でなく «失敗» にする。既定 (kWarn) だと想定外の呼び出しが標準出力に流れるだけで CI は緑のまま通る。
    /// @note 素の mock を StrictMock と同じ扱いにし、緩めたいテストだけ NiceMock<> を明示させる (Docs/conventions/test.md «GoogleMock を使う基準»)。
    /// @note 0=kAllow, 1=kWarn, 2=kFail
    GMOCK_FLAG_SET(default_mock_behavior, 2);

    const int result = RUN_ALL_TESTS();

    /// @note 自分で作ったコンソールは終了と同時に消える。読む前に閉じさせない。
    if (ownsConsole) {
        std::printf("\n===== %s =====\n", result == 0 ? "すべて成功" : "失敗あり");
        fbzz::testkit::WaitForKey();
    }
    return result;
}
