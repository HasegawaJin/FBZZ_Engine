/// @file    ScopeTimer.hpp
/// @brief   RAII スコープタイマー。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// 生成から破棄までの経過時間を計測し、軽量な性能確認に使う。
/// 本格的なプロファイラではなく局所計測用の補助。
#pragma once
#include <Engine/Core/Logger.hpp>
#include <chrono>
#include <string>

namespace fbzz::util {

struct ScopeTimer {
    explicit ScopeTimer(std::string label)
        : m_label(std::move(label))
        , m_start(std::chrono::high_resolution_clock::now()) {}

    ~ScopeTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        float ms = std::chrono::duration<float, std::milli>(end - m_start).count();
        FBZZ_LOG_INFO("[ScopeTimer] %s: %.3f ms", m_label.c_str(), ms);
    }

    ScopeTimer(const ScopeTimer&)            = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;

    // 経過時間を ms で取得 (スコープ終了前に参照したいとき)
    float ElapsedMs() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<float, std::milli>(now - m_start).count();
    }

private:
    std::string m_label;
    std::chrono::high_resolution_clock::time_point m_start;
};

} // namespace fbzz::util
