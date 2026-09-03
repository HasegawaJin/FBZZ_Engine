/// @file    Fixture.hpp
/// @brief   全テストの基底 fixture。Math / Physics 層はこれを使う。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 素の TEST を書かず必ず TEST_F にする、という規約 (Docs/conventions/test.md) の受け皿。
/// 前後処理が要らないテストでもここを継承しておくことで、共通の初期化が必要になったとき
/// 全テストへ一度に配れる。
#pragma once

#include <TestKit/Deterministic.hpp>

#include <gtest/gtest.h>

namespace fbzz::testkit {

class Fixture : public ::testing::Test {
protected:
    void SetUp() override;

    /// テスト名から決まるシードの乱数。同じテストは何度実行しても同じ列になり、
    /// テストごとには別の列になる。失敗したら «そのテストを実行し直すだけ» で再現する。
    DeterministicRng& Rng() { return m_rng; }

private:
    DeterministicRng m_rng{DeterministicRng::kDefaultSeed};
};

} // namespace fbzz::testkit
