/// @file    ScriptApplicationProxy.hpp
/// @brief   Script から Application の基本状態を扱うショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once

#include <cstdint>

namespace fbzz::scene {

class Script;

struct ScriptApplicationProxy {
    Script* script = nullptr;

    void Quit() const;
    bool IsRunning() const;
    uint32_t GetWindowWidth() const;
    uint32_t GetWindowHeight() const;

    /// Play 中なら true。Editor で編集中に走っているスクリプトからは false。
    /// FBZZ_EXECUTE_ALWAYS を付けた Script は編集中も呼ばれるため、入力・物理・音を
    /// 触る処理はこれで囲うこと (編集中はどれも動いていない)。
    [[nodiscard]] bool IsPlaying() const;
    /// IsPlaying() の否定。編集中だけのプレビュー用の値を作る側で読みやすくするための別名。
    [[nodiscard]] bool IsEditMode() const;
};

} // namespace fbzz::scene
