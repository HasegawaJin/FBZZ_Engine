/// @file    AudioBusTests.cpp
/// @brief   ミキサーのバス構成が «親が子より前» へ整列し、壊れた親子関係でも成立することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// バスは親から順に作らないと submix を繋げられない。並びが崩れると
/// «音は鳴るのに音量つまみが効かない» という形で出る (親へ繋がっていない)。
/// 循環や存在しない親はプロジェクト設定を手で編集すれば普通に作れてしまうので、
/// «落ちずに Master 直下へ落とす» ところまで固定する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Audio/AudioBus.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

audio::BusDesc Bus(std::string name, std::string parent = {})
{
    audio::BusDesc desc;
    desc.name   = std::move(name);
    desc.parent = std::move(parent);
    return desc;
}

/// 並びの中での位置。親が子より前にあることを確かめるのに使う。
std::size_t IndexOf(const std::vector<audio::BusDesc>& buses, std::string_view name)
{
    for (std::size_t i = 0; i < buses.size(); ++i)
        if (buses[i].name == name) return i;
    return buses.size();
}

bool Contains(const std::vector<audio::BusDesc>& buses, std::string_view name)
{
    return IndexOf(buses, name) < buses.size();
}

} // namespace

class AudioBusTest : public testkit::EngineFixture {};

/// @name 既定の構成

TEST_F(AudioBusTest, DefaultLayoutStartsWithMaster)
{
    const std::vector<audio::BusDesc> layout = audio::DefaultBusLayout();

    ASSERT_FALSE(layout.empty());
    EXPECT_EQ(layout[audio::kMasterBus].name, audio::kMasterBusName);
    EXPECT_TRUE(layout[audio::kMasterBus].parent.empty());
}

TEST_F(AudioBusTest, DefaultLayoutCoversTheCategories)
{
    const std::vector<audio::BusDesc> layout = audio::DefaultBusLayout();

    EXPECT_TRUE(Contains(layout, "BGM"));
    EXPECT_TRUE(Contains(layout, "SE"));
    EXPECT_TRUE(Contains(layout, "UI"));
    EXPECT_TRUE(Contains(layout, "Voice"));
}

TEST_F(AudioBusTest, DefaultLayoutIsAlreadyNormalised)
{
    const std::vector<audio::BusDesc> layout = audio::DefaultBusLayout();

    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout(layout);

    ASSERT_EQ(normalised.size(), layout.size());
    for (std::size_t i = 0; i < layout.size(); ++i) EXPECT_EQ(normalised[i].name, layout[i].name);
}

/// @name 並べ替え

TEST_F(AudioBusTest, PutsMasterFirstEvenIfItWasDeclaredLast)
{
    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout({
        Bus("SE", audio::kMasterBusName),
        Bus(audio::kMasterBusName),
    });

    ASSERT_FALSE(normalised.empty());
    EXPECT_EQ(normalised[audio::kMasterBus].name, audio::kMasterBusName);
}

TEST_F(AudioBusTest, PlacesEveryParentBeforeItsChild)
{
    /// @note submix は親が先に無いと繋げられない。ここが唯一の順序保証。
    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout({
        Bus("Footsteps", "SE"),
        Bus("SE", audio::kMasterBusName),
        Bus(audio::kMasterBusName),
        Bus("Weapons", "SE"),
    });

    ASSERT_EQ(normalised.size(), 4u);
    EXPECT_LT(IndexOf(normalised, "SE"), IndexOf(normalised, "Footsteps"));
    EXPECT_LT(IndexOf(normalised, "SE"), IndexOf(normalised, "Weapons"));
    EXPECT_LT(IndexOf(normalised, audio::kMasterBusName), IndexOf(normalised, "SE"));
}

TEST_F(AudioBusTest, KeepsDeepChainsInOrder)
{
    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout({
        Bus("D", "C"),
        Bus("C", "B"),
        Bus("B", audio::kMasterBusName),
        Bus(audio::kMasterBusName),
    });

    EXPECT_LT(IndexOf(normalised, "B"), IndexOf(normalised, "C"));
    EXPECT_LT(IndexOf(normalised, "C"), IndexOf(normalised, "D"));
}

/// @name 壊れた構成

TEST_F(AudioBusTest, TreatsAMissingParentAsMaster)
{
    /// @note プロジェクト設定を手で書き換えれば普通に起きる。落とさず拾い上げる。
    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout({
        Bus(audio::kMasterBusName),
        Bus("Orphan", "DoesNotExist"),
    });

    ASSERT_TRUE(Contains(normalised, "Orphan"));
    EXPECT_LT(IndexOf(normalised, audio::kMasterBusName), IndexOf(normalised, "Orphan"));
}

TEST_F(AudioBusTest, BreaksACycleInsteadOfHanging)
{
    /// @note A→B→A。並べ替えが素朴だと無限ループになる場所。
    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout({
        Bus(audio::kMasterBusName),
        Bus("A", "B"),
        Bus("B", "A"),
    });

    EXPECT_TRUE(Contains(normalised, "A"));
    EXPECT_TRUE(Contains(normalised, "B"));
    EXPECT_EQ(normalised[audio::kMasterBus].name, audio::kMasterBusName);
}

TEST_F(AudioBusTest, CollapsesDuplicateNamesToTheLastOne)
{
    std::vector<audio::BusDesc> input{ Bus(audio::kMasterBusName), Bus("SE", audio::kMasterBusName),
                                       Bus("SE", audio::kMasterBusName) };
    input[1].volume = 0.2f;
    input[2].volume = 0.9f;

    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout(input);

    ASSERT_EQ(std::count_if(normalised.begin(), normalised.end(),
                            [](const audio::BusDesc& b) { return b.name == "SE"; }),
              1);
    EXPECT_NEAR(normalised[IndexOf(normalised, "SE")].volume, 0.9f, testkit::kTolerance);
}

TEST_F(AudioBusTest, AddsMasterWhenItIsMissing)
{
    /// @note Master が無い構成は «最終出力が無い» ので鳴らせない。補って成立させる。
    const std::vector<audio::BusDesc> normalised =
        audio::NormalizeBusLayout({ Bus("SE", audio::kMasterBusName) });

    ASSERT_FALSE(normalised.empty());
    EXPECT_EQ(normalised[audio::kMasterBus].name, audio::kMasterBusName);
}

TEST_F(AudioBusTest, AnEmptyLayoutStillYieldsMaster)
{
    const std::vector<audio::BusDesc> normalised = audio::NormalizeBusLayout({});

    ASSERT_FALSE(normalised.empty());
    EXPECT_EQ(normalised[audio::kMasterBus].name, audio::kMasterBusName);
}

TEST_F(AudioBusTest, KeepsTheSettingsOfEachBus)
{
    /// @note 並べ替えで音量や残響の設定が落ちないこと。
    audio::BusDesc se = Bus("SE", audio::kMasterBusName);
    se.volume        = 0.5f;
    se.lowPassCutoff = 0.25f;
    se.reverb        = true;

    const std::vector<audio::BusDesc> normalised =
        audio::NormalizeBusLayout({ Bus(audio::kMasterBusName), se });

    const audio::BusDesc& restored = normalised[IndexOf(normalised, "SE")];
    EXPECT_NEAR(restored.volume, 0.5f, testkit::kTolerance);
    EXPECT_NEAR(restored.lowPassCutoff, 0.25f, testkit::kTolerance);
    EXPECT_TRUE(restored.reverb);
}

TEST_F(AudioBusTest, NormalisingTwiceChangesNothing)
{
    const std::vector<audio::BusDesc> once = audio::NormalizeBusLayout({
        Bus("Footsteps", "SE"),
        Bus("SE", audio::kMasterBusName),
        Bus(audio::kMasterBusName),
    });

    const std::vector<audio::BusDesc> twice = audio::NormalizeBusLayout(once);

    ASSERT_EQ(twice.size(), once.size());
    for (std::size_t i = 0; i < once.size(); ++i) EXPECT_EQ(twice[i].name, once[i].name);
}

} // namespace fbzz::tests
