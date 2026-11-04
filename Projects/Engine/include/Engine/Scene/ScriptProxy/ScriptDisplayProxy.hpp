/// @file    ScriptDisplayProxy.hpp
/// @brief   Script からウィンドウ形態・解像度・垂直同期を操作する。
/// @author  Hasegawa Jin
/// @date    2026-08-23

/// @note app は生死と実行モード、graphics は画質と明るさが担当。表示設定はここに集約する。
/// @note Editor の Play 中は窓の形態・解像度の変更要求が届かない (相手が Editor 自身の窓に
/// @note なるため)。SetFullscreen() の値だけは覚え、IsFullscreen() は Standalone と同じ値を返す。
/// @see Docs/design/game-settings.md
#pragma once
#include <Engine/Api.hpp>

#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Script;

/// @note Option の解像度ドロップダウン用。
struct DisplayResolution {
    uint32_t width  = 0;
    uint32_t height = 0;
};

struct ScriptDisplayProxy {
    Script* script = nullptr;

    /// @name ウィンドウ形態
    /// @{
    /// @note ボーダーレス最大化 ⇔ ウィンドウ。排他フルスクリーンは使わない
    /// @note (スワップチェーンを作り直さずに済み、Alt+Tab も壊れない)。
    FBZZ_ENGINE_API void SetFullscreen(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsFullscreen() const;
    /// @}

    /// @name 解像度
    /// @{
    /// @note ウィンドウモード時のクライアント寸法。フルスクリーン中は「次にウィンドウへ
    /// @note 戻したときの寸法」として覚えるだけで、画面はモニター解像度のまま変わらない。
    FBZZ_ENGINE_API void SetResolution(uint32_t width, uint32_t height) const;
    [[nodiscard]] FBZZ_ENGINE_API uint32_t GetWidth() const;
    [[nodiscard]] FBZZ_ENGINE_API uint32_t GetHeight() const;
    /// @note ウィンドウが載っているモニターの表示領域。
    [[nodiscard]] FBZZ_ENGINE_API DisplayResolution GetMonitorSize() const;
    /// @note モニターが対応する解像度を大きい順に返す。
    [[nodiscard]] FBZZ_ENGINE_API std::vector<DisplayResolution> EnumResolutions() const;
    /// @}

    /// @name 同期
    /// @note 既定は無効。フレームレート上限は time.SetTargetFps() 側で決める。
    /// @{
    FBZZ_ENGINE_API void SetVSync(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool GetVSync() const;
    /// @}
};

} /// @note namespace fbzz::scene
