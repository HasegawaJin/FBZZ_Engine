/// @file    ScriptRuntime.cpp
/// @brief   ScriptProxy 集約ランタイムコンテキストの実装。
/// @author  Hasegawa Jin
/// @date    2026-06-22
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/SceneManager.hpp>

namespace fbzz::scene {

ScriptRuntime* ScriptRuntime::s_override = nullptr;

ScriptRuntime ScriptRuntime::GetCurrent()
{
    if (s_override) {
        // Editor のオーバーライドをそのまま使う。
        // viewportWidth/Height が 0 のときは renderer にフォールバック。
        ScriptRuntime rt = *s_override;
        if ((rt.viewportWidth == 0 || rt.viewportHeight == 0) && rt.renderer) {
            if (rt.viewportWidth  == 0) rt.viewportWidth  = rt.renderer->GetWidth();
            if (rt.viewportHeight == 0) rt.viewportHeight = rt.renderer->GetHeight();
        }
        return rt;
    }

    // Standalone / デフォルト: Application のサブシステムをそのまま使う。
    auto& app = core::Application::Get();
    auto& ren = app.GetRenderer();
    return ScriptRuntime{
        &app.GetSceneManager(),
        &ren,
        ren.GetWidth(),
        ren.GetHeight()
    };
}

void ScriptRuntime::Override(ScriptRuntime* rt)
{
    s_override = rt;
}

ScriptRuntime* ScriptRuntime::GetOverride()
{
    return s_override;
}

} // namespace fbzz::scene
