// FBZZ Engine
// ILogSink.hpp | fbzz::core
// Logger の出力先抽象インターフェース
#pragma once
#include <string>
#include <engine/Core/Logger.hpp>

namespace fbzz::core {

struct LogEntry {
    LogLevel    level;
    std::string message;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void OnLog(const LogEntry& entry) = 0;
};

} // namespace fbzz::core
