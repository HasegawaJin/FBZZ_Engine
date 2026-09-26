/// @file    CrashLogTailTests.cpp
/// @brief   ログの末尾が新しい側だけを古い順に残し、UTF-8 を文字の途中で切らないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
/// @see Docs/design/crash-report.md
#include <TestKit/TestKit.hpp>
#include "Core/CrashLogTail.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {
/// @return 途中で切れた UTF-8 列があれば false。
/// @see https://datatracker.ietf.org/doc/html/rfc3629#section-3 RFC 3629 section 3 UTF-8 definition
bool IsValidUtf8(std::string_view text)
{
    for (size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const size_t length = lead < 0x80u ? 1
                            : (lead & 0xE0u) == 0xC0u ? 2
                            : (lead & 0xF0u) == 0xE0u ? 3
                            : (lead & 0xF8u) == 0xF0u ? 4 : 0;
        if (length == 0 || at + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[at + k]) & 0xC0u) != 0x80u) return false;
        at += length;
    }
    return true;
}
}

class CrashLogTailTest : public testkit::Fixture {
protected:
    core::CrashLogTail m_tail;

    std::vector<std::string> Lines() const
    {
        std::vector<std::string> lines;
        m_tail.ForEachLine([&](const char* line) { lines.emplace_back(line); });
        return lines;
    }

    void AppendNumberedLines(size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            m_tail.OnLog({core::LogLevel::INFO, "tail-line-" + std::to_string(i)});
    }
};

TEST_F(CrashLogTailTest, KeepsOnlyAsManyOfTheNewestLinesAsItsCapacity)
{
    const size_t logged = core::CrashLogTail::LINE_COUNT + 100;
    AppendNumberedLines(logged);

    const auto lines = Lines();

    ASSERT_EQ(lines.size(), core::CrashLogTail::LINE_COUNT);
    EXPECT_EQ(lines.front(), "[INFO]  tail-line-100");
    EXPECT_EQ(lines.back(), "[INFO]  tail-line-" + std::to_string(logged - 1));
}

TEST_F(CrashLogTailTest, WritesTheRetainedLinesOldestFirst)
{
    const size_t logged = core::CrashLogTail::LINE_COUNT * 2 + 17;
    AppendNumberedLines(logged);

    const auto lines = Lines();

    ASSERT_EQ(lines.size(), core::CrashLogTail::LINE_COUNT);
    for (size_t i = 0; i < lines.size(); ++i)
        EXPECT_EQ(lines[i], "[INFO]  tail-line-" + std::to_string(logged - lines.size() + i));
}

TEST_F(CrashLogTailTest, TruncatesALongLineOnACharacterBoundary)
{
    /// @note 実行文字セットに依存せず、3 バイト文字 U+3042 で上限を超える。
    std::string message = "utf8-tail-";
    for (size_t i = 0; i < core::CrashLogTail::LINE_BYTES; ++i)
        message += "\xE3\x81\x82";

    m_tail.OnLog({core::LogLevel::INFO, message});
    const auto lines = Lines();

    ASSERT_EQ(lines.size(), 1u);
    const std::string expected = "[INFO]  " + message;
    EXPECT_LT(lines[0].size(), core::CrashLogTail::LINE_BYTES);
    EXPECT_LT(lines[0].size(), expected.size());
    EXPECT_EQ(expected.compare(0, lines[0].size(), lines[0]), 0);
    EXPECT_TRUE(IsValidUtf8(lines[0]));
}

TEST_F(CrashLogTailTest, ClearDiscardsEarlierLinesAndAllowsNewLogging)
{
    AppendNumberedLines(core::CrashLogTail::LINE_COUNT + 1);

    m_tail.Clear();

    EXPECT_TRUE(Lines().empty());
    m_tail.OnLog({core::LogLevel::WARNING, "after-clear"});
    EXPECT_EQ(Lines(), (std::vector<std::string>{"[WARN]  after-clear"}));
}
}
