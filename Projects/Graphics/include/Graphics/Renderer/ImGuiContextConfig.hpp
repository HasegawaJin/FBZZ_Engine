/// @file    ImGuiContextConfig.hpp
/// @brief   ImGui DLL 内の context 選択を CPU スレッドごとに分離する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

struct ImGuiContext;

#if defined(FBZZ_IMGUI_EXPORTS)
#define FBZZ_IMGUI_CONTEXT_API __declspec(dllexport)
#elif defined(FBZZ_IMGUI_IMPORTS)
#define FBZZ_IMGUI_CONTEXT_API __declspec(dllimport)
#else
#define FBZZ_IMGUI_CONTEXT_API
#endif

namespace fbzz::renderer {
/// @note DLL ごとに TLS を複製せず、共有 ImGui DLL が各スレッドの非所有 context を保持する。
/// @see https://github.com/ocornut/imgui/blob/v1.92.8/imgui.cpp Context and Memory Allocators.
FBZZ_IMGUI_CONTEXT_API ImGuiContext*& ImGuiContextForThread();
}

#define GImGui fbzz::renderer::ImGuiContextForThread()
