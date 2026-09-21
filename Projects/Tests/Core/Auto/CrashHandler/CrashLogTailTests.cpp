/// @file    CrashLogTailTests.cpp
/// @brief   レポートに載るログの末尾が、新しい側だけを古い順に残し、UTF-8 を文字の途中で切らないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
/// @see Docs/design/crash-report.md
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/CrashHandler.hpp>
#include <Engine/Core/Logger.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fbzz::tests {
namespace {

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// @brief 先頭バイトが申告した長さだけ継続バイト (10xxxxxx) が続いているかを見る。
/// @return 途中で切れた列が 1 つでもあれば false。
/// @see https://datatracker.ietf.org/doc/html/rfc3629#section-3 RFC 3629 §3 UTF-8 definition
[[nodiscard]] bool IsValidUtf8(std::string_view text)
{
    for (size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const size_t length = lead < 0x80u              ? 1
                            : (lead & 0xE0u) == 0xC0u   ? 2
                            : (lead & 0xF0u) == 0xE0u   ? 3
                            : (lead & 0xF8u) == 0xF0u   ? 4
                                                        : 0;
        if (length == 0 || at + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[at + k]) & 0xC0u) != 0x80u) return false;
        at += length;
    }
    return true;
}

/// @brief 見出しが申告する «残す行数»。輪の大きさをテストへ写さずに済ませる。
/// @return 見出しが無ければ 0。
[[nodiscard]] size_t ParseKeptLineCount(const std::string& report)
{
    constexpr std::string_view HEAD = "--- log (oldest first, last ";
    const size_t begin = report.find(HEAD);
    if (begin == std::string::npos) return 0;
    return std::strtoul(report.c_str() + begin + HEAD.size(), nullptr, 10);
}

[[nodiscard]] size_t CountOccurrences(const std::string& text, std::string_view needle)
{
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

/// @brief `needle` を含む行を、`needle` の位置から行末まで返す。`[INFO]  ` の飾りと CR を外す。
[[nodiscard]] std::string FindLoggedLine(const std::string& report, std::string_view needle)
{
    std::istringstream lines(report);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t begin = line.find(needle);
        if (begin == std::string::npos) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line.substr(begin);
    }
    return {};
}

} // namespace

class CrashLogTailTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        /// @note 末尾に載ることを見るので INFO を通す。TearDown で既定へ戻る。
        SetLogLevel(core::LogLevel::INFO);
        ASSERT_TRUE(temp.IsValid());
        ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashLogTailTest"));
    }

    void TearDown() override
    {
        /// @note Uninstall が sink を外して輪も空にする。次のテストへ持ち越さない。
        core::CrashHandler::Uninstall();
        EngineFixture::TearDown();
    }

    testkit::TempDir temp{ "CrashLogTail" };
};

TEST_F(CrashLogTailTest, KeepsOnlyAsManyOfTheNewestLinesAsTheReportAnnounces)
{
    constexpr int LOGGED = 300;
    for (int i = 0; i < LOGGED; ++i)
        core::Logger::Info("tail-line-%03d", i);

    const std::filesystem::path dir = core::CrashHandler::WriteReport("log tail size");

    ASSERT_FALSE(dir.empty());
    const std::string report = ReadAll(dir / "report.txt");
    const size_t kept = ParseKeptLineCount(report);
    ASSERT_GT(kept, 0u) << "レポートに «残す行数» の見出しが無い";
    ASSERT_LT(kept, static_cast<size_t>(LOGGED)) << "輪の大きさを超えて流さないと «捨てる» ことを確かめられない";
    EXPECT_EQ(CountOccurrences(report, "tail-line-"), kept);
    EXPECT_EQ(report.find("tail-line-000"), std::string::npos);
    EXPECT_NE(report.find("tail-line-299"), std::string::npos);
}

TEST_F(CrashLogTailTest, WritesTheRetainedLinesOldestFirst)
{
    for (int i = 0; i < 300; ++i)
        core::Logger::Info("tail-line-%03d", i);

    const std::filesystem::path dir = core::CrashHandler::WriteReport("log tail order");

    ASSERT_FALSE(dir.empty());
    const std::string report   = ReadAll(dir / "report.txt");
    const size_t      older    = report.find("tail-line-298");
    const size_t      newer    = report.find("tail-line-299");
    ASSERT_NE(older, std::string::npos);
    ASSERT_NE(newer, std::string::npos);
    EXPECT_LT(older, newer);
}

TEST_F(CrashLogTailTest, TruncatesALongLineOnACharacterBoundary)
{
    /// @note 3 バイト文字 (U+3042) だけを並べ、輪の 1 行を必ず超えさせる。
    ///       リテラルをバイトで書くのは、実行文字セットの設定に結果を左右させないため。
    std::string message = "utf8-tail-";
    for (int i = 0; i < 200; ++i)
        message += "\xE3\x81\x82";

    core::Logger::Info("%s", message.c_str());
    const std::filesystem::path dir = core::CrashHandler::WriteReport("utf8 tail");

    ASSERT_FALSE(dir.empty());
    const std::string kept = FindLoggedLine(ReadAll(dir / "report.txt"), "utf8-tail-");
    ASSERT_FALSE(kept.empty());
    EXPECT_LT(kept.size(), message.size()) << "切り詰めが起きないと «途中で切らない» を確かめられない";
    EXPECT_EQ(message.compare(0, kept.size(), kept), 0) << "残った側は元の先頭と一致すること";
    EXPECT_TRUE(IsValidUtf8(kept)) << "文字の途中で切れている";
}

} // namespace fbzz::tests
