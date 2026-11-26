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
struct AllocationInfo {
    void*       pointer = nullptr;         ///< 確保した領域の先頭アドレス。台帳は領域を所有しない。
    std::size_t size = 0;                  ///< 要求した領域の大きさ（バイト）。
    std::size_t alignment = 0;             ///< 確保先の先頭に要求したアラインメント（バイト）。
    MemoryTag   tag = MemoryTag::UNKNOWN;  ///< 使用量を集計する用途区分。
    const char* allocatorName = "Unknown"; ///< 確保元の表示名。文字列は呼び出し側が保持する。
    const char* file = "Unknown";          ///< 確保を要求したソースファイル名。文字列は呼び出し側が保持する。
    int         line = 0;                  ///< 確保を要求したソース行番号。
    std::uint64_t allocationId = 0;        ///< 台帳への登録時に割り当てる連番。リセット後は 1 から始まる。
    bool        isActive = false;          ///< 台帳が未解放の確保として追跡している間は true。
};

} // namespace fbzz::core
