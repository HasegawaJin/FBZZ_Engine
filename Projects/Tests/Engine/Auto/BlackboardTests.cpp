/// @file    BlackboardTests.cpp
/// @brief   AI の共有記憶が型を守り、書き込み時刻を覚え、未設定を区別することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 敵の «いま何を知っているか» は全部ここに乗る。型不一致を黙って通すと、
/// float の枠へ入れた bool が «常に true» として読まれる。書き込み時刻が
/// 進まないと「5 秒見失ったら警戒を解く」が永久に発火しない ── どちらも
/// 「敵の動きがなんか変」としか見えない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/AI/Blackboard.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

ai::BlackboardDef Define(std::string name, ai::BlackboardType type)
{
    ai::BlackboardDef def;
    def.name = std::move(name);
    def.type = type;
    return def;
}

/// 型ごとに 1 枠ずつ持つ盤面。添字は宣言順にそのままキーになる。
enum Key : ai::BlackboardKey { kBool = 0, kInt, kFloat, kVector, kEntity, kString };

std::vector<ai::BlackboardDef> MixedLayout()
{
    return {
        Define("flag", ai::BlackboardType::Bool),
        Define("count", ai::BlackboardType::Int),
        Define("ratio", ai::BlackboardType::Float),
        Define("spot", ai::BlackboardType::Vector3),
        Define("target", ai::BlackboardType::Entity),
        Define("label", ai::BlackboardType::String),
    };
}

} // namespace

class BlackboardTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        board.Reset(MixedLayout());
    }

    ai::Blackboard board;
};

// --- 領域 -------------------------------------------------------------------

TEST_F(BlackboardTest, ResetCreatesOneEntryPerDefinition)
{
    EXPECT_EQ(board.Size(), MixedLayout().size());
    EXPECT_TRUE(board.IsValidKey(kString));
    EXPECT_FALSE(board.IsValidKey(kString + 1));
}

TEST_F(BlackboardTest, RemembersTheDeclaredTypeOfEachKey)
{
    EXPECT_EQ(board.TypeOf(kBool), ai::BlackboardType::Bool);
    EXPECT_EQ(board.TypeOf(kVector), ai::BlackboardType::Vector3);
}

TEST_F(BlackboardTest, ResetReplacesTheWholeLayout)
{
    board.Reset({ Define("only", ai::BlackboardType::Float) });

    EXPECT_EQ(board.Size(), 1u);
    EXPECT_FALSE(board.IsValidKey(1));
}

// --- 読み書き ---------------------------------------------------------------

TEST_F(BlackboardTest, ReadsBackEveryTypeItWrote)
{
    const scene::EntityID entity{ 7, 2 };
    EXPECT_TRUE(board.SetBool(kBool, true));
    EXPECT_TRUE(board.SetInt(kInt, -5));
    EXPECT_TRUE(board.SetFloat(kFloat, 0.25f));
    EXPECT_TRUE(board.SetVector3(kVector, { 1.0f, 2.0f, 3.0f }));
    EXPECT_TRUE(board.SetEntity(kEntity, entity));
    EXPECT_TRUE(board.SetString(kString, "alerted"));

    bool flag = false;
    int count = 0;
    float ratio = 0.0f;
    math::Vector3 spot;
    scene::EntityID target;
    std::string label;

    EXPECT_TRUE(board.GetBool(kBool, flag));
    EXPECT_TRUE(board.GetInt(kInt, count));
    EXPECT_TRUE(board.GetFloat(kFloat, ratio));
    EXPECT_TRUE(board.GetVector3(kVector, spot));
    EXPECT_TRUE(board.GetEntity(kEntity, target));
    EXPECT_TRUE(board.GetString(kString, label));

    EXPECT_TRUE(flag);
    EXPECT_EQ(count, -5);
    EXPECT_NEAR(ratio, 0.25f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(spot, math::Vector3(1.0f, 2.0f, 3.0f), testkit::kTolerance);
    EXPECT_EQ(target, entity);
    EXPECT_EQ(label, "alerted");
}

TEST_F(BlackboardTest, OverwritingKeepsTheLatestValue)
{
    board.SetInt(kInt, 1);
    board.SetInt(kInt, 2);

    int count = 0;
    ASSERT_TRUE(board.GetInt(kInt, count));
    EXPECT_EQ(count, 2);
}

// --- 型の守り -------------------------------------------------------------

TEST_F(BlackboardTest, RefusesToWriteTheWrongType)
{
    // アセットとスクリプトが別々に編集された結果のオーサリングミス。
    // assert で落とさず false を返し、値も書き換えない。
    ASSERT_TRUE(board.SetFloat(kFloat, 1.5f));

    EXPECT_FALSE(board.SetBool(kFloat, true));
    EXPECT_FALSE(board.SetInt(kFloat, 3));

    float ratio = 0.0f;
    ASSERT_TRUE(board.GetFloat(kFloat, ratio));
    EXPECT_NEAR(ratio, 1.5f, testkit::kTolerance);
}

TEST_F(BlackboardTest, RefusesToReadTheWrongTypeAndLeavesTheOutputAlone)
{
    board.SetFloat(kFloat, 1.5f);

    int count = 42;
    EXPECT_FALSE(board.GetInt(kFloat, count));
    EXPECT_EQ(count, 42);
}

TEST_F(BlackboardTest, RejectsKeysOutsideTheLayout)
{
    const ai::BlackboardKey missing = static_cast<ai::BlackboardKey>(board.Size() + 10);

    EXPECT_FALSE(board.SetBool(missing, true));

    bool flag = false;
    EXPECT_FALSE(board.GetBool(missing, flag));
}

// --- 未設定の区別 -----------------------------------------------------------

TEST_F(BlackboardTest, StartsWithNothingWritten)
{
    // 「まだ一度も見ていない」と「見て false だった」は別の状態。
    EXPECT_FALSE(board.IsSet(kBool));
    EXPECT_EQ(board.GetLastWriteTick(kBool), 0u);
    EXPECT_NEAR(board.GetLastWriteTime(kBool), 0.0f, testkit::kTolerance);
}

TEST_F(BlackboardTest, MarksAKeyAsSetOnceItIsWritten)
{
    board.SetBool(kBool, false);   // 値が false でも «書かれた» ことは残る

    EXPECT_TRUE(board.IsSet(kBool));
}

TEST_F(BlackboardTest, ResetClearsTheWrittenFlags)
{
    board.SetBool(kBool, true);

    board.Reset(MixedLayout());

    EXPECT_FALSE(board.IsSet(kBool));
}

TEST_F(BlackboardTest, ARejectedWriteDoesNotMarkTheKeyAsSet)
{
    EXPECT_FALSE(board.SetInt(kFloat, 3));

    EXPECT_FALSE(board.IsSet(kFloat));
}

// --- 書き込み時刻 -----------------------------------------------------------

TEST_F(BlackboardTest, StampsTheCurrentTickAndTimeOnWrite)
{
    // 「最後にプレイヤーを見てから 5 秒」を各ノードが自前タイマーで持たずに済ませるための値。
    board.SetTick(12);
    board.SetTime(3.5f);

    board.SetVector3(kVector, { 1.0f, 0.0f, 0.0f });

    EXPECT_EQ(board.GetLastWriteTick(kVector), 12u);
    EXPECT_NEAR(board.GetLastWriteTime(kVector), 3.5f, testkit::kTolerance);
}

TEST_F(BlackboardTest, TheStampMovesForwardWithEachWrite)
{
    board.SetTick(1);
    board.SetTime(0.1f);
    board.SetFloat(kFloat, 1.0f);

    board.SetTick(60);
    board.SetTime(1.0f);
    board.SetFloat(kFloat, 2.0f);

    EXPECT_EQ(board.GetLastWriteTick(kFloat), 60u);
    EXPECT_NEAR(board.GetLastWriteTime(kFloat), 1.0f, testkit::kTolerance);
}

TEST_F(BlackboardTest, ReadingDoesNotRefreshTheStamp)
{
    // 読んだだけで «新しい情報» になってしまうと、記憶の減衰が働かない。
    board.SetTick(5);
    board.SetTime(0.5f);
    board.SetFloat(kFloat, 1.0f);

    board.SetTick(600);
    board.SetTime(10.0f);
    float ratio = 0.0f;
    ASSERT_TRUE(board.GetFloat(kFloat, ratio));

    EXPECT_EQ(board.GetLastWriteTick(kFloat), 5u);
}

TEST_F(BlackboardTest, TracksTheClockIndependentlyOfWrites)
{
    board.SetTick(30);
    board.SetTime(0.5f);

    EXPECT_EQ(board.GetTick(), 30u);
    EXPECT_NEAR(board.GetTime(), 0.5f, testkit::kTolerance);
}

// --- 予約キー ---------------------------------------------------------------

TEST_F(BlackboardTest, ReservedKeysSitAtTheirFixedIndices)
{
    // PerceptionSystem は名前検索を避けてこの添字へ直接書く。
    // 並びが変わると «別のキーへ書き込む» ことになる。
    const std::vector<ai::BlackboardDef>& reserved = ai::ReservedBlackboardDefs();

    ASSERT_EQ(reserved.size(), ai::bb::ReservedCount);
    EXPECT_EQ(reserved[ai::bb::TargetEntity].type, ai::BlackboardType::Entity);
    EXPECT_EQ(reserved[ai::bb::TargetPosition].type, ai::BlackboardType::Vector3);
    EXPECT_EQ(reserved[ai::bb::HasTarget].type, ai::BlackboardType::Bool);
    EXPECT_EQ(reserved[ai::bb::Health01].type, ai::BlackboardType::Float);
}

TEST_F(BlackboardTest, ReservedKeysAreMarkedReserved)
{
    // エディタは予約キーの名前変更と削除を禁じる。その印がここで立っている。
    for (const ai::BlackboardDef& def : ai::ReservedBlackboardDefs())
        EXPECT_TRUE(def.reserved) << def.name;
}

} // namespace fbzz::tests
