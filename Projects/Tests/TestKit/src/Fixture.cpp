/// @file    Fixture.cpp
/// @brief   基底 fixture の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Fixture.hpp>

#include <cstdint>
#include <string>

namespace fbzz::testkit {

namespace {

std::uint64_t Fnv1a(const std::string& text)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

void Fixture::SetUp()
{
    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string          name = info ? std::string(info->test_suite_name()) + "." + info->name()
                                           : std::string("unknown");

    const std::uint64_t seed = Fnv1a(name);
    m_rng                    = DeterministicRng(seed);

    /// @note 失敗した XML / JSON レポートからシードを読めるようにする。
    RecordProperty("rng_seed", std::to_string(seed));
}

} // namespace fbzz::testkit
