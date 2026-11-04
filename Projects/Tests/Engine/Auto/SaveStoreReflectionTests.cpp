/// @file    SaveStoreReflectionTests.cpp
/// @brief   反射 callback の再入による保存先と読込みスナップショットの失効を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-01
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <Engine/Util/SaveStore.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <functional>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

struct ReentrantSaveValue final : scene::IScriptSerializable {
    int m_first = 11;
    int m_second = 22;
    std::string m_name = "original nested value";
    std::vector<int> m_values{3, 5, 7};
    std::function<void()> m_callback;

    void Reflect(scene::IReflector& reflector) override
    {
        reflector.Field("first", m_first);
        reflector.BeginObject("nested");
        if (m_callback) m_callback();
        reflector.Field("second", m_second);
        reflector.Field("name", m_name);
        reflector.ListField("values", m_values);
        reflector.EndObject();
    }
};

struct SparseSaveValue final : scene::IScriptSerializable {
    int m_first = 303;
    void Reflect(scene::IReflector& reflector) override { reflector.Field("first", m_first); }
};

void ExpectValue(const ReentrantSaveValue& actual, const ReentrantSaveValue& expected)
{
    EXPECT_EQ(actual.m_first, expected.m_first);
    EXPECT_EQ(actual.m_second, expected.m_second);
    EXPECT_EQ(actual.m_name, expected.m_name);
    EXPECT_EQ(actual.m_values, expected.m_values);
}

ReentrantSaveValue ReplacementValue()
{
    ReentrantSaveValue value;
    value.m_first = 101;
    value.m_second = 202;
    value.m_name = "replacement nested value";
    value.m_values = {17, 19};
    return value;
}

} /// @note namespace

class SaveStoreReflectionTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"SaveStoreReflection"};
};

TEST_F(SaveStoreReflectionTest, WriteKeepsTheKeySelectedBeforeReflection)
{
    util::SaveStore store("unused.toml");
    std::string key = "save-a";
    ReentrantSaveValue source;
    source.m_callback = [&] { key.back() = 'b'; };

    ASSERT_TRUE(store.Write(key, source));

    EXPECT_TRUE(store.Has("save-a"));
    EXPECT_FALSE(store.Has("save-b"));
    ReentrantSaveValue loaded;
    ASSERT_TRUE(store.Read("save-a", loaded));
    ExpectValue(loaded, source);
}

TEST_F(SaveStoreReflectionTest, ReadKeepsNestedValuesWhenReflectionReplacesTheSameKey)
{
    util::SaveStore store("unused.toml");
    ReentrantSaveValue source;
    auto replacement = ReplacementValue();
    ASSERT_TRUE(store.Write("profile", source));
    auto loaded = ReplacementValue();
    loaded.m_callback = [&] { EXPECT_TRUE(store.Write("profile", replacement)); };

    ASSERT_TRUE(store.Read("profile", loaded));

    ExpectValue(loaded, source);
    ReentrantSaveValue current;
    ASSERT_TRUE(store.Read("profile", current));
    ExpectValue(current, replacement);
}

TEST_F(SaveStoreReflectionTest, ReadKeepsItsSnapshotAcrossNestedReadAndClear)
{
    util::SaveStore store("unused.toml");
    ReentrantSaveValue source;
    auto other = ReplacementValue();
    ASSERT_TRUE(store.Write("profile", source));
    ASSERT_TRUE(store.Write("other", other));
    ReentrantSaveValue nested;
    auto loaded = ReplacementValue();
    loaded.m_callback = [&] {
        EXPECT_TRUE(store.Read("other", nested));
        store.Clear();
        store.SetString("survivor", "new state");
    };

    ASSERT_TRUE(store.Read("profile", loaded));

    ExpectValue(loaded, source);
    ExpectValue(nested, other);
    EXPECT_EQ(store.Keys(), (std::vector<std::string>{"survivor"}));
    EXPECT_EQ(store.GetString("survivor"), "new state");
}

TEST_F(SaveStoreReflectionTest, MissingFieldsAndNonTableSourcesPreserveDefaults)
{
    util::SaveStore store("unused.toml");
    SparseSaveValue sparse;
    ASSERT_TRUE(store.Write("profile", sparse));
    auto loaded = ReplacementValue();
    auto expected = loaded;
    expected.m_first = sparse.m_first;

    ASSERT_TRUE(store.Read("profile", loaded));

    ExpectValue(loaded, expected);
    int calls = 0;
    loaded.m_callback = [&] { ++calls; };
    store.SetInt("scalar", 5);
    EXPECT_FALSE(store.Read("scalar", loaded));
    EXPECT_FALSE(store.Read("missing", loaded));
    EXPECT_EQ(calls, 0);
    ExpectValue(loaded, expected);
}

TEST_F(SaveStoreReflectionTest, ReadKeepsItsSnapshotWhenReflectionLoadsAnotherDiskRevision)
{
    ASSERT_TRUE(m_temp.IsValid());
    util::SaveStore store(util::FileSystem::PathToUtf8(m_temp.File("save.toml")));
    ReentrantSaveValue disk;
    ASSERT_TRUE(store.Write("profile", disk));
    ASSERT_TRUE(store.Save());
    auto memory = ReplacementValue();
    ASSERT_TRUE(store.Write("profile", memory));
    ReentrantSaveValue loaded;
    loaded.m_callback = [&] { EXPECT_TRUE(store.Load()); };

    ASSERT_TRUE(store.Read("profile", loaded));

    ExpectValue(loaded, memory);
    ReentrantSaveValue current;
    ASSERT_TRUE(store.Read("profile", current));
    ExpectValue(current, disk);
    EXPECT_FALSE(store.IsDirty());
}

} /// @note namespace fbzz::tests
