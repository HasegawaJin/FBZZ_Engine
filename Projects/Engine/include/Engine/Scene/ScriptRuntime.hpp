/// @file    ScriptRuntime.hpp
/// @brief   Editor と Standalone で ScriptProxy が参照するサブシステムを集約するコンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-06-22

/// @note Editor は独自の SceneManager/描画ビューポートを持ち、Standalone は core::Application のサブシステムをそのまま使う。ScriptProxy が直接 Application::Get() を呼ぶと Editor 側のインスタンスと食い違うため、すべてのアクセスをこの struct 経由に一本化する。
#pragma once
#include <Engine/Api.hpp>
#include <cstdint>

namespace fbzz::scene  { class SceneManager; }
namespace fbzz::renderer { class IRenderer; }

namespace fbzz::scene {

struct ScriptRuntime {
    using QuitHandler = void (*)(void*, uint64_t);
    using DisplayHandler = bool (*)(void*, uint64_t, uint32_t, uint32_t, uint32_t);
    using PointerQuery = bool (*)(void*, uint64_t);
    SceneManager*        sceneManager   = nullptr;
    renderer::IRenderer* renderer       = nullptr;
    /// @note ゲームビューポートのピクセルサイズ。0 のとき GetCurrent() 内で renderer->GetWidth/Height() を使う。Editor ではビューポートがウィンドウより小さいため、WorldToScreenPoint 等の座標変換に正しいサイズが必要。
    uint32_t             viewportWidth  = 0;
    uint32_t             viewportHeight = 0;
    /// @note 非所有 host hook。owner は解除してから破棄する。callback は終了 intent だけを保持し Scene を即時破棄しない。
    QuitHandler          requestQuit = nullptr;
    void*                quitContext = nullptr;
    /// @note 所有 host が frame 境界で適用する表示 intent。Editor は登録しても window 変更を受け付けない。
    DisplayHandler       requestDisplay = nullptr;
    void*                displayContext = nullptr;
    PointerQuery         queryPointerOverUI = nullptr;
    void*                pointerContext = nullptr;
    [[nodiscard]] FBZZ_ENGINE_API static bool IsPointerOverUI(uint64_t sceneLifetime);
    FBZZ_ENGINE_API static bool RequestDisplay(uint64_t sceneLifetime, uint32_t operation, uint32_t first, uint32_t second = 0);
    /// @return 非 Play または Scene lifetime=0 は false。Standalone は終了 flag、host は次の安全な frame で処理する。
    FBZZ_ENGINE_API static bool RequestQuit(uint64_t sceneLifetime);

    /// @brief 現在のランタイムを値で返す。
    /// @return Override が設定されていれば Override、なければ Application のデフォルトを構築して返す。
    FBZZ_ENGINE_API static ScriptRuntime  GetCurrent();

    /// @brief Editor が Play 開始/終了時に呼ぶ。
    /// @param rt nullptr でデフォルト (Standalone) に戻す。
    FBZZ_ENGINE_API static void           Override(ScriptRuntime* rt);

    /// @brief 設定済みの Override ポインタを返す。Play 中に viewportWidth 等を更新するために使う。
    FBZZ_ENGINE_API static ScriptRuntime* GetOverride();

private:
    static ScriptRuntime* s_override;
};

} /// @note namespace fbzz::scene
