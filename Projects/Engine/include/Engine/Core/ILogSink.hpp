// FBZZ Engine
// ILogSink.hpp | fbzz::core
// Logger の出力先抽象インターフェース
// コンソールやエディタパネルなど、ログの表示先を非所有ポインタで登録する。
// 出力先の寿命は登録側が管理し、破棄前に RemoveSink する。
#pragma once
#include <string>
#include <Engine/Core/Logger.hpp>

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
