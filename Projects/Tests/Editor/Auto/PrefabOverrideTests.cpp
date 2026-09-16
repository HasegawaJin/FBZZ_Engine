/// @file    PrefabOverrideTests.cpp
/// @brief   プレハブ差分のプロパティパス解決と、差分一覧の取り回し。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// オーバーライドは «インスタンス側だけ変えた値» を指す。パス解決が 1 段でも
/// ずれると、Apply したときに «変えていないはずの値» が本体へ書き戻るか、
/// 逆に «変えたはずの値» が黙って捨てられる。どちらもシーンを開き直すまで気づけない。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/PrefabOverrides.hpp>

#include <string>

namespace fbzz::tests {
namespace {

using editor::FormatNodeForDisplay;
using editor::PrefabOverride;
using editor::PrefabOverrideSet;
using scene::FindNodeAtPath;
using scene::SetNodeAtPath;

// transform.position.x のように 3 段で辿れるテーブルを作る。
toml::table MakeNestedTable()
{
    toml::table position;
    position.insert("x", 1.0);
    position.insert("y", 2.0);

    toml::table transform;
    transform.insert("position", std::move(position));

    toml::table root;
    root.insert("name", std::string("Player"));
    root.insert("active", true);
    root.insert("transform", std::move(transform));
    return root;
}

PrefabOverride MakeOverride(const char* sourceId, const char* path)
{
    PrefabOverride entry;
    entry.prefabSourceId = sourceId;
    entry.path           = path;
    return entry;
}

} // namespace

// --- パス解決 (読み) --------------------------------------------------------

TEST(PrefabPathLookup, FindsATopLevelKey)
{
    const toml::table table = MakeNestedTable();

    const toml::node* node = FindNodeAtPath(table, "name");

    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->value_or(std::string{}), "Player");
}

TEST(PrefabPathLookup, FindsANestedKey)
{
    const toml::table table = MakeNestedTable();

    const toml::node* node = FindNodeAtPath(table, "transform.position.x");

    ASSERT_NE(node, nullptr);
    EXPECT_DOUBLE_EQ(node->value_or(0.0), 1.0);
}

TEST(PrefabPathLookup, FindsAnIntermediateTable)
{
    const toml::table table = MakeNestedTable();

    const toml::node* node = FindNodeAtPath(table, "transform.position");

    ASSERT_NE(node, nullptr);
    EXPECT_NE(node->as_table(), nullptr);
}

TEST(PrefabPathLookup, ReturnsNullForAMissingKey)
{
    const toml::table table = MakeNestedTable();

    EXPECT_EQ(FindNodeAtPath(table, "missing"), nullptr);
    EXPECT_EQ(FindNodeAtPath(table, "transform.rotation"), nullptr);
    EXPECT_EQ(FindNodeAtPath(table, "transform.position.z"), nullptr);
}

TEST(PrefabPathLookup, ReturnsNullWhenAMidSegmentIsNotATable)
{
    // "name" は文字列。その先を辿ろうとしたら «無い» と答えるしかない。
    const toml::table table = MakeNestedTable();

    EXPECT_EQ(FindNodeAtPath(table, "name.x"), nullptr);
}

TEST(PrefabPathLookup, ReturnsNullForAnEmptyPath)
{
    const toml::table table = MakeNestedTable();

    EXPECT_EQ(FindNodeAtPath(table, ""), nullptr);
}

// --- パス解決 (書き) --------------------------------------------------------

TEST(PrefabPathAssign, ReplacesATopLevelValue)
{
    toml::table table = MakeNestedTable();
    const toml::value<std::string> replacement{ std::string("Enemy") };

    ASSERT_TRUE(SetNodeAtPath(table, "name", replacement));

    EXPECT_EQ(table["name"].value_or(std::string{}), "Enemy");
}

TEST(PrefabPathAssign, ReplacesANestedValue)
{
    toml::table table = MakeNestedTable();
    const toml::value<double> replacement{ 42.0 };

    ASSERT_TRUE(SetNodeAtPath(table, "transform.position.x", replacement));

    EXPECT_DOUBLE_EQ(table["transform"]["position"]["x"].value_or(0.0), 42.0);
}

TEST(PrefabPathAssign, LeavesSiblingsAlone)
{
    // 1 つ書き換えたときに隣の値まで消えると、差分適用で他の項目が既定値へ戻る。
    toml::table table = MakeNestedTable();
    const toml::value<double> replacement{ 42.0 };

    ASSERT_TRUE(SetNodeAtPath(table, "transform.position.x", replacement));

    EXPECT_DOUBLE_EQ(table["transform"]["position"]["y"].value_or(0.0), 2.0);
    EXPECT_EQ(table["name"].value_or(std::string{}), "Player");
}

TEST(PrefabPathAssign, AddsAMissingLeafKey)
{
    // 末尾のキーは無くても作る (プレハブに無い項目をインスタンスが持つ場合)。
    toml::table table = MakeNestedTable();
    const toml::value<double> value{ 3.0 };

    ASSERT_TRUE(SetNodeAtPath(table, "transform.position.z", value));

    EXPECT_DOUBLE_EQ(table["transform"]["position"]["z"].value_or(0.0), 3.0);
}

TEST(PrefabPathAssign, ChangesTheValueType)
{
    // 型が変わっても «置き換え» として通ること (bool だった項目が文字列になる等)。
    toml::table table = MakeNestedTable();
    const toml::value<std::string> replacement{ std::string("yes") };

    ASSERT_TRUE(SetNodeAtPath(table, "active", replacement));

    EXPECT_EQ(table["active"].value_or(std::string{}), "yes");
}

TEST(PrefabPathAssign, RefusesToCreateMissingIntermediateTables)
{
    // 途中のテーブルまで作ると «想定外の形» を静かに生やす。ここは失敗させる。
    toml::table table = MakeNestedTable();
    const toml::value<double> value{ 1.0 };

    EXPECT_FALSE(SetNodeAtPath(table, "physics.mass", value));
    EXPECT_EQ(FindNodeAtPath(table, "physics"), nullptr);
}

TEST(PrefabPathAssign, RefusesWhenAMidSegmentIsNotATable)
{
    toml::table table = MakeNestedTable();
    const toml::value<double> value{ 1.0 };

    EXPECT_FALSE(SetNodeAtPath(table, "name.x", value));
}

TEST(PrefabPathAssign, RoundTripsThroughFind)
{
    toml::table table = MakeNestedTable();
    const toml::value<double> value{ 7.5 };

    ASSERT_TRUE(SetNodeAtPath(table, "transform.position.y", value));

    const toml::node* node = FindNodeAtPath(table, "transform.position.y");
    ASSERT_NE(node, nullptr);
    EXPECT_DOUBLE_EQ(node->value_or(0.0), 7.5);
}

// --- 差分一覧 ---------------------------------------------------------------

TEST(PrefabOverrideSetOps, RemovesTheMatchingEntry)
{
    PrefabOverrideSet set;
    set.prefabAssetPath = "Assets/Prefabs/Enemy.prefab";
    set.entries.push_back(MakeOverride("go-1", "transform.position.x"));
    set.entries.push_back(MakeOverride("go-1", "name"));

    const PrefabOverrideSet result =
        editor::WithoutEntry(set, MakeOverride("go-1", "name"));

    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].path, "transform.position.x");
}

TEST(PrefabOverrideSetOps, KeepsEntriesWithTheSamePathOnAnotherObject)
{
    // パスだけで消すと、別オブジェクトの同名プロパティまで巻き添えになる。
    PrefabOverrideSet set;
    set.entries.push_back(MakeOverride("go-1", "name"));
    set.entries.push_back(MakeOverride("go-2", "name"));

    const PrefabOverrideSet result =
        editor::WithoutEntry(set, MakeOverride("go-1", "name"));

    ASSERT_EQ(result.entries.size(), 1u);
    EXPECT_EQ(result.entries[0].prefabSourceId, "go-2");
}

TEST(PrefabOverrideSetOps, KeepsTheAssetPathAndInstanceTables)
{
    PrefabOverrideSet set;
    set.prefabAssetPath = "Assets/Prefabs/Enemy.prefab";
    set.instanceTables["go-1"] = toml::table{};
    set.entries.push_back(MakeOverride("go-1", "name"));

    const PrefabOverrideSet result =
        editor::WithoutEntry(set, MakeOverride("go-1", "name"));

    EXPECT_EQ(result.prefabAssetPath, "Assets/Prefabs/Enemy.prefab");
    EXPECT_EQ(result.instanceTables.size(), 1u);
    EXPECT_TRUE(result.Empty());
}

TEST(PrefabOverrideSetOps, RemovingAnAbsentEntryChangesNothing)
{
    PrefabOverrideSet set;
    set.entries.push_back(MakeOverride("go-1", "name"));

    const PrefabOverrideSet result =
        editor::WithoutEntry(set, MakeOverride("go-9", "nope"));

    EXPECT_EQ(result.entries.size(), 1u);
}

TEST(PrefabOverrideSetOps, EmptyReflectsTheEntryCountOnly)
{
    PrefabOverrideSet set;
    EXPECT_TRUE(set.Empty());

    set.instanceTables["go-1"] = toml::table{};
    EXPECT_TRUE(set.Empty());   // テーブルがあっても差分が無ければ空

    set.entries.push_back(MakeOverride("go-1", "name"));
    EXPECT_FALSE(set.Empty());
}

// --- 表示整形 ---------------------------------------------------------------

TEST(PrefabOverrideDisplay, ShowsAPlaceholderForAMissingNode)
{
    EXPECT_EQ(FormatNodeForDisplay(nullptr), "(none)");
}

TEST(PrefabOverrideDisplay, RendersScalarValues)
{
    const toml::value<std::string> text{ std::string("Player") };
    const toml::value<double>      number{ 1.5 };
    const toml::value<bool>        flag{ true };

    EXPECT_FALSE(FormatNodeForDisplay(&text).empty());
    EXPECT_FALSE(FormatNodeForDisplay(&number).empty());
    EXPECT_FALSE(FormatNodeForDisplay(&flag).empty());
}

TEST(PrefabOverrideDisplay, CollapsesAValueOntoOneLine)
{
    // 差分一覧は 1 行で並ぶ。改行が残ると行が崩れて後続が読めなくなる。
    toml::table inner;
    inner.insert("x", 1.0);
    inner.insert("y", 2.0);
    inner.insert("z", 3.0);

    const std::string shown = FormatNodeForDisplay(&inner);

    EXPECT_EQ(shown.find('\n'), std::string::npos) << shown;
    EXPECT_EQ(shown.find('\t'), std::string::npos) << shown;
}

TEST(PrefabOverrideDisplay, TruncatesALongValue)
{
    const toml::value<std::string> text{ std::string(500, 'x') };

    const std::string shown = FormatNodeForDisplay(&text);

    EXPECT_LT(shown.size(), 200u) << shown.size();
}

TEST(PrefabOverrideDisplay, DoesNotCutInTheMiddleOfAMultiByteCharacter)
{
    // バイトで切ると日本語の値が «□» で終わる。文字境界まで戻すこと。
    std::string japanese;
    for (int i = 0; i < 60; ++i) japanese += "あ";   // 3 バイト x 60
    const toml::value<std::string> text{ japanese };

    const std::string shown = FormatNodeForDisplay(&text);

    // 末尾が UTF-8 の継続バイト (0b10xxxxxx) で終わっていないこと。
    ASSERT_FALSE(shown.empty());
    const auto last = static_cast<unsigned char>(shown.back());
    EXPECT_NE(last & 0xC0, 0x80) << shown;
}

} // namespace fbzz::tests
