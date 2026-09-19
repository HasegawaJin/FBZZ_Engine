/// @file    EngineFixture.hpp
/// @brief   Engine の静的な状態を触るテスト向けの基底 fixture。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// Logger は静的な最小レベルと非所有 sink の一覧をプロセス全体で共有している。
/// 前のテストが変えたまま抜けると、後続のテストの結果が実行順で変わる。
/// ここで «入る前に整え、出るときに戻す» を一箇所に閉じ込める。
#pragma once

#include <TestKit/Fixture.hpp>

#include <Engine/Core/Logger.hpp>

namespace fbzz::testkit {

class EngineFixture : public Fixture {
protected:
    void SetUp() override;
    void TearDown() override;

    /// テスト中だけログを出したいときに呼ぶ。TearDown で既定へ戻る。
    void SetLogLevel(core::LogLevel level);

private:
    /// Logger に getter が無いため、規約上の既定値 (INFO) へ戻す。
    static constexpr core::LogLevel kDefaultLevel = core::LogLevel::INFO;
};

} // namespace fbzz::testkit
