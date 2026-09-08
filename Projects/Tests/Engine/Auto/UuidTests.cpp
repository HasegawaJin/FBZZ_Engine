/// @file    UuidTests.cpp
/// @brief   UUID v4 の書式・バージョン / バリアント・一意性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// GameObject の instanceId はここで作られ、IK の Pole/Target のような
/// オブジェクト間参照の永続キーになる。重複すると «別のオブジェクトを掴む»、
/// 書式が崩れると保存済みシーンの参照が引けなくなる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/Uuid.hpp>

#include <set>
#include <string>

namespace fbzz::tests {

class UuidTest : public testkit::EngineFixture {};

TEST_F(UuidTest, HasTheCanonicalLayout)
{
    const std::string id = util::GenerateUUID();

    ASSERT_EQ(id.size(), 36u);
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[18], '-');
    EXPECT_EQ(id[23], '-');
}

TEST_F(UuidTest, UsesOnlyLowerCaseHexOutsideTheSeparators)
{
    const std::string id = util::GenerateUUID();

    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;

        const char c = id[i];
        const bool digit = c >= '0' && c <= '9';
        const bool hex   = c >= 'a' && c <= 'f';
        EXPECT_TRUE(digit || hex) << "位置 " << i << " が '" << c << "'";
    }
}

TEST_F(UuidTest, MarksVersionFourAndTheRfcVariant)
{
    // 版とバリアントが立っていないと、外部ツールから «UUID ではない» と扱われる。
    const std::string id = util::GenerateUUID();

    EXPECT_EQ(id[14], '4');
    const char variant = id[19];
    EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b')
        << "variant = '" << variant << "'";
}

TEST_F(UuidTest, GeneratesADifferentValueEveryTime)
{
    // 同じフレームで何十個も作る (プレファブの一括生成)。そこで衝突すると
    // «参照が別のオブジェクトを指す» という形で出る。
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) seen.insert(util::GenerateUUID());

    EXPECT_EQ(seen.size(), 1000u);
}

} // namespace fbzz::tests
