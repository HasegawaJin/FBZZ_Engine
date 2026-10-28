/// @file    ImGuiContextStorage.cpp
/// @brief   共有 ImGui DLL のスレッド別 context 参照を所有する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Renderer/ImGuiContextConfig.hpp>

namespace fbzz::renderer {
ImGuiContext*& ImGuiContextForThread()
{
    static thread_local ImGuiContext* context = nullptr;
    return context;
}
} /// @note namespace fbzz::renderer
