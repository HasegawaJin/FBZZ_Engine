/// @file    ManagedPrimitiveWidgets.cpp
/// @brief   Lossless primitive widgets and enum masks from the authoritative managed field schema.
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Editor/Util/ManagedPrimitiveWidgets.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Op/OpData.hpp>
#include <imgui.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace fbzz::editor {
namespace {
std::string Text(const OpData& schema, const char* key) { const auto* data = schema.Find(key); return data && data->IsString() ? data->AsString() : std::string{}; }
bool Unsigned(std::string_view kind) { return kind == "uint8" || kind == "uint16" || kind == "uint" || kind == "uint64" || kind == "char"; }
int Width(std::string_view kind) { return kind.ends_with('8') ? 8 : kind.ends_with("16") || kind == "char" ? 16 : kind == "int" || kind == "uint" ? 32 : 64; }
uint64_t Mask(std::string_view kind) { const int width = Width(kind); return width == 64 ? UINT64_MAX : (uint64_t{1} << width) - 1; }
bool Bits(std::string_view kind, const OpData& data, uint64_t& result)
{
    if (kind == "uint64") {
        if (!data.IsString()) return false; const auto& text = data.AsString();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::to_string(result) == text;
    }
    int64_t number = 0;
    if (kind == "int64") {
        if (!data.IsString()) return false; const auto& text = data.AsString();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || std::to_string(number) != text) return false;
    } else { if (!data.IsInt()) return false; number = data.AsInt64(); }
    if (Unsigned(kind)) { if (number < 0 || static_cast<uint64_t>(number) > Mask(kind)) return false; result = static_cast<uint64_t>(number); }
    else {
        const int width = Width(kind);
        if (width != 64 && (number < -(int64_t{1} << (width - 1)) || number > (int64_t{1} << (width - 1)) - 1)) return false;
        result = std::bit_cast<uint64_t>(number) & Mask(kind);
    }
    return true;
}
OpData Value(std::string_view kind, uint64_t bits)
{
    bits &= Mask(kind);
    if (Unsigned(kind)) return kind == "uint64" ? OpData(std::to_string(bits)) : OpData(static_cast<int64_t>(bits));
    if (Width(kind) < 64 && (bits & (uint64_t{1} << (Width(kind) - 1)))) bits |= ~Mask(kind);
    const auto number = std::bit_cast<int64_t>(bits);
    return kind == "int64" ? OpData(std::to_string(number)) : OpData(number);
}
bool Enum(ImGuiReflector& ui, const char* label, const OpData& schema, OpData& value)
{
    const std::string kind = Text(schema, "numericKind"); uint64_t current = 0;
    const auto* candidates = schema.Find("enumValues");
    if (!candidates || !candidates->IsArray() || !Bits(kind, value, current)) return true;
    if (!ui.BeginRow(label)) return true;
    const auto* flag = schema.Find("flags"); const bool flags = flag && flag->IsBool() && flag->AsBool();
    std::string preview = value.IsString() ? value.AsString() : std::to_string(value.AsInt64());
    for (const auto& candidate : candidates->AsArray()) { uint64_t bits = 0; const auto* number = candidate.Find("value"); if (number && Bits(kind, *number, bits) && bits == current) { preview = Text(candidate, "name"); break; } }
    if (ImGui::BeginCombo("##primitive", preview.c_str())) {
        for (const auto& candidate : candidates->AsArray()) {
            uint64_t bits = 0; const auto* number = candidate.Find("value"); if (!number || !Bits(kind, *number, bits)) continue;
            const std::string name = Text(candidate, "name");
            if (flags) {
                bool selected = bits == 0 ? current == 0 : (current & bits) == bits;
                if (ImGui::Checkbox(name.c_str(), &selected)) { current = bits == 0 ? (selected ? 0 : current) : selected ? current | bits : current & ~bits; value = Value(kind, current); ui.m_changed = true; }
            } else if (ImGui::Selectable(name.c_str(), current == bits)) { current = bits; value = *number; ui.m_changed = true; }
        }
        ImGui::EndCombo();
    }
    ui.EndRow(); return true;
}
template<class T> bool Integer(ImGuiReflector& ui, const char* label, const OpData& schema, std::string_view kind, ImGuiDataType dataType, OpData& value)
{
    uint64_t bits = 0; if (!Bits(kind, value, bits)) { ImGui::TextDisabled("%s: 型の範囲外です", label); return true; }
    T number = Unsigned(kind) ? static_cast<T>(bits) : static_cast<T>(std::bit_cast<int64_t>(bits | ((Width(kind) < 64 && (bits & (uint64_t{1} << (Width(kind) - 1)))) ? ~Mask(kind) : 0)));
    if (!ui.BeginRow(label)) return true;
    if (ImGui::InputScalar("##primitive", dataType, &number)) {
        /// @note Schema bounds only constrain the visible draft; Host normalization validates the final typed candidate.
        if (const auto* minimum = schema.Find("minimum"); minimum) { uint64_t limit = 0; if (Bits(kind, *minimum, limit)) {
            const T bound = Unsigned(kind) ? static_cast<T>(limit) : static_cast<T>(std::bit_cast<int64_t>(limit | ((Width(kind) < 64 && (limit & (uint64_t{1} << (Width(kind) - 1)))) ? ~Mask(kind) : 0))); number = std::max(number, bound); } }
        if (const auto* maximum = schema.Find("maximum"); maximum) { uint64_t limit = 0; if (Bits(kind, *maximum, limit)) {
            const T bound = Unsigned(kind) ? static_cast<T>(limit) : static_cast<T>(std::bit_cast<int64_t>(limit | ((Width(kind) < 64 && (limit & (uint64_t{1} << (Width(kind) - 1)))) ? ~Mask(kind) : 0))); number = std::min(number, bound); } }
        value = Value(kind, static_cast<uint64_t>(number)); ui.m_changed = true;
    }
    if (kind == "char") { ImGui::SameLine(); ImGui::TextDisabled("UTF-16 U+%04X", static_cast<unsigned>(number)); }
    ui.EndRow(); return true;
}
} /// @note namespace
bool TryReadManagedPrimitiveReal(const OpData& value, double& output)
{
    if (!value.IsInt() && !value.IsFloat()) return false;
    const double number = value.IsInt() ? static_cast<double>(value.AsInt64()) : value.AsDouble();
    if (!std::isfinite(number)) return false;
    output = number; return true;
}
bool DrawManagedPrimitive(ImGuiReflector& ui, const char* label, const OpData& schema, OpData& value)
{
    const std::string kind = Text(schema, "valueKind");
    if (kind == "enum") return Enum(ui, label, schema, value);
    if (kind == "int8") return Integer<int8_t>(ui, label, schema, kind, ImGuiDataType_S8, value);
    if (kind == "uint8") return Integer<uint8_t>(ui, label, schema, kind, ImGuiDataType_U8, value);
    if (kind == "int16") return Integer<int16_t>(ui, label, schema, kind, ImGuiDataType_S16, value);
    if (kind == "uint16" || kind == "char") return Integer<uint16_t>(ui, label, schema, kind, ImGuiDataType_U16, value);
    if (kind == "int") return Integer<int32_t>(ui, label, schema, kind, ImGuiDataType_S32, value);
    if (kind == "uint") return Integer<uint32_t>(ui, label, schema, kind, ImGuiDataType_U32, value);
    if (kind == "int64") return Integer<int64_t>(ui, label, schema, kind, ImGuiDataType_S64, value);
    if (kind == "uint64") return Integer<uint64_t>(ui, label, schema, kind, ImGuiDataType_U64, value);
    if (kind == "decimal") { std::string text = value.IsString() ? value.AsString() : std::string{}; ui.Field(label, text); if (ui.m_changed) value = OpData(std::move(text)); return true; }
    if (kind == "float" || kind == "double") {
        double number = 0; if (!TryReadManagedPrimitiveReal(value, number)) return true;
        if (!ui.BeginRow(label)) return true; bool changed = false;
        if (kind == "float") { float single = static_cast<float>(number); if (std::isfinite(single)) { changed = ImGui::InputScalar("##primitive", ImGuiDataType_Float, &single, nullptr, nullptr, "%.9g"); number = single; } }
        else changed = ImGui::InputScalar("##primitive", ImGuiDataType_Double, &number, nullptr, nullptr, "%.17g");
        if (changed && std::isfinite(number)) {
            double bound = 0;
            if (const auto* minimum = schema.Find("minimum"); minimum && TryReadManagedPrimitiveReal(*minimum, bound)) number = std::max(number, bound);
            if (const auto* maximum = schema.Find("maximum"); maximum && TryReadManagedPrimitiveReal(*maximum, bound)) number = std::min(number, bound);
            value = kind == "float" ? OpData(static_cast<float>(number)) : OpData(number); ui.m_changed = true;
        }
        ui.EndRow(); return true;
    }
    return false;
}
}
