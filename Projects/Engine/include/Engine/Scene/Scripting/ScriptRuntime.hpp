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
    SceneManager*        sceneManager   = nullptr; ///< Script がシーンを操作するホストの SceneManager の非所有参照。
    renderer::IRenderer* renderer       = nullptr; ///< ScriptProxy が描画状態を照会するレンダラーの非所有参照。
    uint32_t             viewportWidth  = 0; ///< ゲームビューポートのピクセルサイズ。0 のとき GetCurrent() 内で renderer->GetWidth/Height() を使う。Editor ではビューポートがウィンドウより小さいため、WorldToScreenPoint 等の座標変換に正しいサイズが必要。
    uint32_t             viewportHeight = 0; ///< ゲームビューポートの高さ [px]。0 の場合はレンダラーから得る。
    QuitHandler          requestQuit = nullptr; ///< 非所有 host hook。owner は解除してから破棄する。callback は終了 intent だけを保持し Scene を即時破棄しない。
    void*                quitContext = nullptr; ///< 終了要求のフックに渡す非所有ホストコンテキスト。
    DisplayHandler       requestDisplay = nullptr; ///< 所有 host が frame 境界で適用する表示 intent。Editor は登録しても window 変更を受け付けない。
    void*                displayContext = nullptr; ///< 表示設定のフックに渡す非所有ホストコンテキスト。
    PointerQuery         queryPointerOverUI = nullptr; ///< 現在のポインターが対象シーンの UI 上にあるか問い合わせる非所有フック。
    void*                pointerContext = nullptr; ///< ポインター照会のフックに渡す非所有ホストコンテキスト。
    [[nodiscard]] FBZZ_ENGINE_API static bool IsPointerOverUI(uint64_t sceneLifetime);
    FBZZ_ENGINE_API static bool RequestDisplay(uint64_t sceneLifetime, uint32_t operation, uint32_t first, uint32_t second = 0);
    /// @return 非 Play または Scene lifetime=0 は false。Standalone は終了 flag、host は次の安全な frame で処理する。
    FBZZ_ENGINE_API static bool RequestQuit(uint64_t sceneLifetime);

    /// @brief 現在のランタイムを値で返す。
    /// @return Override が設定されていれば Override、なければ Application のデフォルトを構築して返す。
    FBZZ_ENGINE_API static ScriptRuntime  GetCurrent();

    /// @brief Editor が Play 開始/終了時に呼ぶ。
    /// @param[in] rt nullptr でデフォルト (Standalone) に戻す。
    FBZZ_ENGINE_API static void           Override(ScriptRuntime* rt);

    /// @brief 設定済みの Override ポインタを返す。Play 中に viewportWidth 等を更新するために使う。
    FBZZ_ENGINE_API static ScriptRuntime* GetOverride();

private:
    static ScriptRuntime* s_override; ///< Editor などが差し替えた実行コンテキストの非所有参照。
};

} // namespace fbzz::scene
