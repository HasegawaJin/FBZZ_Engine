/// @file    TempDir.cpp
/// @brief   一時ディレクトリの生成と後始末。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/TempDir.hpp>

#include <atomic>
#include <string>
#include <system_error>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fbzz::testkit {

namespace {

/// @note CTest は同じユーザーで別プロセスを並列実行するため、PID と連番の両方で分離する。
std::atomic<unsigned> g_counter{0};

}

TempDir::TempDir(const std::string& label)
{
    std::error_code ec;
    const auto      base = std::filesystem::temp_directory_path(ec);
    if (ec) return;

    const auto prefix = "fbzz_test_" + label + "_" + std::to_string(GetCurrentProcessId()) + "_";
    for (;;) {
        const unsigned serial = g_counter.fetch_add(1, std::memory_order_relaxed);
        m_path = base / (prefix + std::to_string(serial));
        /// @note PID 再利用後の残骸も削除しない。作成に成功したディレクトリだけを所有する。
        m_valid = std::filesystem::create_directory(m_path, ec);
        if (m_valid || ec) return;
    }
}

TempDir::~TempDir()
{
    if (!m_valid) return;
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
}

}
