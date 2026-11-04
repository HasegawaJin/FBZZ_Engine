/// @file    ScriptApplicationProxy.hpp
/// @brief   Script から Application の基本状態を扱うショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once
#include <Engine/Api.hpp>

#include <cstdint>

namespace fbzz::scene {

class Script;

struct ScriptApplicationProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void Quit() const;
    FBZZ_ENGINE_API bool IsRunning() const;
    FBZZ_ENGINE_API uint32_t GetWindowWidth() const;
    FBZZ_ENGINE_API uint32_t GetWindowHeight() const;

    /// @note Play 中なら true。Editor で編集中に走っているスクリプトからは false。
    /// @note ExecuteAlways の Script は編集中も呼ばれるため、入力・物理・音を
    /// @note 触る処理はこれで囲うこと (編集中はどれも動いていない)。
    [[nodiscard]] FBZZ_ENGINE_API bool IsPlaying() const;
    /// @note IsPlaying() の否定。編集中だけのプレビュー用の値を作る側で読みやすくするための別名。
    [[nodiscard]] FBZZ_ENGINE_API bool IsEditMode() const;
};

} /// @note namespace fbzz::scene
