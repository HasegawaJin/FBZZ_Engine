/// @file    EditorPlayLog.hpp
/// @brief   Per-session Standalone Play log includes startup failures before the runtime module exists.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/Logger.hpp>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fbzz::editor_launcher {

class EditorPlayLog final : public core::ILogSink {
public:
    explicit EditorPlayLog(const std::filesystem::path& path)
    {
        if (path.empty()) return;
        m_file.open(path, std::ios::out | std::ios::trunc);
        if (m_file) core::Logger::AddSink(this);
    }
    ~EditorPlayLog() override { if (m_file.is_open()) core::Logger::RemoveSink(this); }
    [[nodiscard]] bool IsOpen() const { return m_file.is_open(); }
    void OnLog(const core::LogEntry& entry) override
    {
        const std::lock_guard lock(m_mutex);
        const char* level = "INFO";
        switch (entry.level) {
        case core::LogLevel::DEBUG: level = "DEBUG"; break;
        case core::LogLevel::INFO: level = "INFO"; break;
        case core::LogLevel::WARNING: level = "WARN"; break;
        case core::LogLevel::LOG_ERROR: level = "ERROR"; break;
        }
        m_file << '[' << level << "] " << entry.message << '\n';
        m_file.flush();
    }
private:
    std::ofstream m_file;
    std::mutex m_mutex;
};

} /// @note namespace fbzz::editor_launcher
