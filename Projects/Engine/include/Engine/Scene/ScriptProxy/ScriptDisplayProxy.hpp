/// @file    ScriptDisplayProxy.hpp
/// @brief   Script からウィンドウ形態・解像度・垂直同期を操作する。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 設計意図 (WHY app と分けるか):
///   app は「アプリの生死と実行モード」を答える場所。表示設定を混ぜると
///   IsPlaying() を探すのに 20 個の setter を読むことになる。
///
///   フレームレート上限は time.SetTargetFps() に既にあるのでここには置かない。
///   画質と明るさは graphics プロキシ。
///
/// Editor で Play しているあいだ、窓の形態と解像度を変える要求は届かない。
/// 相手が Editor 自身の窓になってしまうためで、Editor は常にウィンドウモード。
/// SetFullscreen() の値だけは覚えており、IsFullscreen() は Standalone と同じ答えを返す。
///
/// 詳細は Docs/design/game-settings.md。
#pragma once

#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Script;

/// Option の解像度ドロップダウン用。
struct DisplayResolution {
    uint32_t width  = 0;
    uint32_t height = 0;
};

struct ScriptDisplayProxy {
    Script* script = nullptr;

    /// @name ウィンドウ形態
    ///@{
    /// ボーダーレス最大化 ⇔ ウィンドウ。排他フルスクリーンは使わない
    /// (スワップチェーンを作り直さずに済み、Alt+Tab も壊れない)。
    void SetFullscreen(bool enabled) const;
    [[nodiscard]] bool IsFullscreen() const;
    ///@}

    /// @name 解像度
    ///@{
    /// ウィンドウモード時のクライアント寸法。フルスクリーン中は「次にウィンドウへ
    /// 戻したときの寸法」として覚えるだけで、画面はモニター解像度のまま変わらない。
    void SetResolution(uint32_t width, uint32_t height) const;
    [[nodiscard]] uint32_t GetWidth() const;
    [[nodiscard]] uint32_t GetHeight() const;
    /// ウィンドウが載っているモニターの表示領域。
    [[nodiscard]] DisplayResolution GetMonitorSize() const;
    /// モニターが対応する解像度を大きい順に返す。
    [[nodiscard]] std::vector<DisplayResolution> EnumResolutions() const;
    ///@}

    /// @name 同期
    /// 既定は無効。フレームレート上限は time.SetTargetFps() 側で決める。
    ///@{
    void SetVSync(bool enabled) const;
    [[nodiscard]] bool GetVSync() const;
    ///@}
};

} // namespace fbzz::scene
