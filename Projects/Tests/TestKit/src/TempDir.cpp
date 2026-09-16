/// @file    TempDir.cpp
/// @brief   一時ディレクトリの生成と後始末。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/TempDir.hpp>

#include <atomic>
#include <string>
#include <system_error>

namespace fbzz::testkit {

namespace {

/// 同一プロセス内で名前が衝突しないための連番。
/// プロセス間の衝突は temp_directory_path 側のユーザー分離に任せる。
std::atomic<unsigned> g_counter{0};

} // namespace

TempDir::TempDir(const std::string& label)
{
    std::error_code ec;
    const auto      base = std::filesystem::temp_directory_path(ec);
    if (ec) return;

    const unsigned serial = g_counter.fetch_add(1, std::memory_order_relaxed);
    m_path = base / ("fbzz_test_" + label + "_" + std::to_string(serial));

    std::filesystem::remove_all(m_path, ec);
    m_valid = std::filesystem::create_directories(m_path, ec) && !ec;
}

TempDir::~TempDir()
{
    if (!m_valid) return;
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
}

} // namespace fbzz::testkit
