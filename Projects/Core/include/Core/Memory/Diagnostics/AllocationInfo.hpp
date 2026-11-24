/// @file    AllocationInfo.hpp
/// @brief   メモリリーク特定用の割り当て詳細情報。
/// @author  Hasegawa Jin
/// @date    2026-06-01
/// @note 未解放ポインタのサイズ・用途・発生位置を保持し、終了時やデバッグ UI で原因を追えるようにする。
#pragma once

#include <cstddef>
#include <cstdint>

namespace fbzz::core {

/// @brief メモリ使用量を分類する固定タグ。
/// @note 動的な文字列マップを使わず、エンジン初期段階でも追加アロケーションなしで集計できるようにする。
enum class MemoryTag : std::size_t {
    UNKNOWN = 0,
    CORE,
    RENDERER,
    SCENE,
    PHYSICS,
    AUDIO,
    ASSET,
    EDITOR,
    COUNT
};

/// @brief 1 回の Allocate に対応する追跡情報。
/// @note pointer が nullptr なら空スロット。file/line は呼び出し元を特定するための非所有文字列参照。
struct AllocationInfo {
    void*       pointer = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 0;
    MemoryTag   tag = MemoryTag::UNKNOWN;
    const char* allocatorName = "Unknown";
    const char* file = "Unknown";
    int         line = 0;
    std::uint64_t allocationId = 0;
    bool        isActive = false;
};

} /// @note namespace fbzz::core
