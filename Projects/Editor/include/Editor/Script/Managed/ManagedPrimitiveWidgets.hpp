/// @file    ManagedPrimitiveWidgets.hpp
/// @brief   Shared exact primitive presentation for managed Scene and DataAsset settings.
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
namespace fbzz::editor {
struct ImGuiReflector;
class OpData;
/// @return False for nonnumeric/nonfinite input, leaving output unchanged; integer tokens are converted explicitly.
bool TryReadManagedPrimitiveReal(const OpData& value, double& output);
/// @return True for a numeric/enum schema; candidate validation remains in the shared Host settings transaction.
/// @note I64/U64/decimal values retain their canonical strings and char remains a UInt16 code unit.
bool DrawManagedPrimitive(ImGuiReflector& ui, const char* label, const OpData& schema, OpData& value);
}
