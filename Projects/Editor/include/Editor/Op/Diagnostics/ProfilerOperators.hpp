/// @file    ProfilerOperators.hpp
/// @brief   Profiler の収集操作と履歴 Query の登録。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
namespace fbzz::editor {
class OperatorRegistry;
void RegisterProfilerOperators(OperatorRegistry& registry);
}
