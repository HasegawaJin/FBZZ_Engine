/// @file    EditorPlayLog.hpp
/// @brief   Standalone Play ごとにログを開き、ランタイム初期化前の失敗も記録する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <Engine/Core/Diagnostics/ILogSink.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fbzz::editor_launcher {

class EditorPlayLog final : public core::ILogSink {
public:
    explicit EditorPlayLog(const std::filesystem::path& path)
    {
        if (path.empty()) {
            return;
        }
        m_file.open(path, std::ios::out | std::ios::trunc);
        if (m_file) {
            core::Logger::AddSink(this);
        }
    }
    ~EditorPlayLog() override {
        if (m_file.is_open()) {
            core::Logger::RemoveSink(this);
        }
    }
    [[nodiscard]] bool IsOpen() const { return m_file.is_open(); }
    void OnLog(const core::LogEntry& entry) override
    {
        // 複数スレッドからの書き込みが一行の途中で混ざらないようにする。
        const std::lock_guard lock(m_mutex);
        const char* level = "INFO";
        switch (entry.level) {
        case core::LogLevel::DEBUG: level = "DEBUG"; break;
        case core::LogLevel::INFO: level = "INFO"; break;
        case core::LogLevel::WARNING: level = "WARN"; break;
        case core::LogLevel::LOG_ERROR: level = "ERROR"; break;
        }

        // 親 Editor が起動失敗直前の診断も読めるよう、ログを一行ずつフラッシュする。
        m_file << '[' << level << "] " << entry.message << '\n';
        m_file.flush();
    }
private:
    std::ofstream m_file; ///< 現在の Standalone Play セッション専用の出力ストリーム。
    std::mutex m_mutex; ///< 複数スレッドから届くログを一行単位で同期する。
};

} // namespace fbzz::editor_launcher
