// FBZZ Engine
// TaskSystemSubmitTests.cpp | GoogleTest
// TaskSystem の future 結果と引数転送を自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Concurrency/TaskSystem.hpp>

namespace fbzz::tests {

class TaskSystemSubmitTest : public ::testing::Test {
protected:
    void SetUp() override { fbzz::TaskSystem::Init(2); }
    void TearDown() override { fbzz::TaskSystem::Shutdown(); }
};

TEST_F(TaskSystemSubmitTest, ReturnsTheResultOfAValueTask)
{
    auto result = fbzz::TaskSystem::Submit([] { return 42; });
    EXPECT_EQ(result.get(), 42);
}

TEST_F(TaskSystemSubmitTest, ForwardsArgumentsToTheTask)
{
    auto result = fbzz::TaskSystem::Submit([](int left, int right) {
        return left * right;
    }, 6, 7);

    EXPECT_EQ(result.get(), 42);
}

TEST_F(TaskSystemSubmitTest, SupportsVoidTasks)
{
    bool completed = false;
    auto result = fbzz::TaskSystem::Submit([&completed] { completed = true; });
    result.get();

    EXPECT_TRUE(completed);
}

} // namespace fbzz::tests
