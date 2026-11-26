/// @file    EngineFixture.cpp
/// @brief   Engine 向け基底 fixture の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Engine/EngineFixture.hpp>

namespace fbzz::testkit {

void EngineFixture::SetUp()
{
    Fixture::SetUp();

    // Logger は sink とは別に stdout へも直接書く。既定のままだと Engine の INFO ログが
    // gtest の出力に混ざり、失敗箇所が埋もれる。ERROR だけ通す。
    core::Logger::SetMinLevel(core::LogLevel::LOG_ERROR);
}

void EngineFixture::TearDown()
{
    core::Logger::SetMinLevel(kDefaultLevel);
}

void EngineFixture::SetLogLevel(core::LogLevel level)
{
    core::Logger::SetMinLevel(level);
}

} // namespace fbzz::testkit
