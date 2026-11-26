/// @file    ILogSink.hpp
/// @brief   Logger の出力先抽象インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note コンソールやエディタパネルなど、ログの表示先を非所有ポインタで登録する。
/// @note 出力先の寿命は登録側が管理し、破棄前に RemoveSink する。
#pragma once
#include <string>
#include <Core/Logger/Logger.hpp>

namespace fbzz::core {

/// @brief ログ出力先へ渡す 1 件分の重要度と本文。
struct LogEntry {
    LogLevel    level;   ///< 出力先が絞り込みに使うログの重要度。
    std::string message; ///< 書式展開後の本文。呼び出し位置付き API ではファイル名と行番号も含む。
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    /// @param[in] entry この呼び出し中に参照できるログ。後で使う内容は出力先がコピーする。
    virtual void OnLog(const LogEntry& entry) = 0;
};

} // namespace fbzz::core
