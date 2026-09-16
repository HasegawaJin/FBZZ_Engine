/// @file    ScriptRuntime.hpp
/// @brief   Editor と Standalone で ScriptProxy が参照するサブシステムを集約するコンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// WHY: Editor は独自の SceneManager / 描画ビューポートを持ち、
/// Standalone は core::Application のサブシステムをそのまま使う。
/// ScriptProxy の各メソッドが直接 Application::Get() を呼ぶと
/// Editor 側のインスタンスと食い違い、バグの温床になる。
/// すべての ScriptProxy アクセスをこの struct 経由にすることで
/// 発散を1箇所に封じ込め、テスト・拡張を容易にする。
#pragma once
#include <cstdint>

namespace fbzz::scene  { class SceneManager; }
namespace fbzz::renderer { class IRenderer; }

namespace fbzz::scene {

struct ScriptRuntime {
    SceneManager*        sceneManager   = nullptr;
    renderer::IRenderer* renderer       = nullptr;
    // ゲームビューポートのピクセルサイズ。
    // 0 のとき GetCurrent() 内で renderer->GetWidth/Height() を使う。
    // WHY: Editor ではゲームビューポートがウィンドウより小さいため、
    //      WorldToScreenPoint 等の座標変換に正しいサイズが必要。
    uint32_t             viewportWidth  = 0;
    uint32_t             viewportHeight = 0;

    // 現在のランタイムを値で返す。
    // Override が設定されていれば Override を、なければ Application のデフォルトを構築して返す。
    static ScriptRuntime  GetCurrent();

    // Editor が Play 開始/終了時に呼ぶ。nullptr でデフォルト (Standalone) に戻す。
    static void           Override(ScriptRuntime* rt);

    // 設定済みの Override ポインタを返す。Play 中に viewportWidth 等を更新するために使う。
    static ScriptRuntime* GetOverride();

private:
    static ScriptRuntime* s_override;
};

} // namespace fbzz::scene
