/// @file    WaterfallFluidBake.hpp
/// @brief   Connected waterfall bake progress and editor operations.
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <string>
#include <string_view>

namespace fbzz::editor {
struct EditorContext;
class OperatorRegistry;

void RegisterWaterfallFluidOperators(OperatorRegistry& registry);
void PollWaterfallFluidBake(EditorContext& ctx);
[[nodiscard]] std::string WaterfallFluidBakeStatus(std::string_view node);
} /// @note namespace fbzz::editor
