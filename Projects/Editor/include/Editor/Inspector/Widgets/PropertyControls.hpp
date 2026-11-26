/// @file    PropertyControls.hpp
/// @brief   手書きのフィールドを共通のラベル列へ配置する。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#pragma once
#include <Editor/UI/Widgets/ImGuiWidgets.hpp>
#include <utility>

namespace fbzz::editor::property {

inline std::string Caption(const char* label)
{
    std::string caption = label;
    if (const auto marker = caption.find("##"); marker != std::string::npos) {
        caption.resize(marker);
    }
    return LOCT(caption.c_str());
}

/// @note 既存の ImGui ID を保持し、編集中のフォーカスとパネルの設定を継続する。
template<typename DrawControl>
bool Draw(const char* label, DrawControl drawControl)
{
    if (label[0] == '#' && label[1] == '#') {
        return drawControl(label);
    }

    ImGui::PushID(label);
    const auto row = widgets::BeginPropertyRow();
    ImGui::PopID();
    widgets::PlacePropertyValue(widgets::DrawPropertyLabel(Caption(label).c_str()));
    const std::string id = std::string("###") + label;
    const bool changed = drawControl(id.c_str());
    widgets::EndPropertyRow(row);
    return changed;
}

template<typename... Args>
bool Checkbox(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::Checkbox(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool Combo(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::Combo(id, std::forward<Args>(args)...); });
}

inline bool DragFloat(const char* label, float* value, float speed = 1.0f, float min = 0, float max = 0,
                       const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::DragFloat(id, value, speed, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

inline bool DragFloat2(const char* label, float* value, float speed = 1.0f, float min = 0, float max = 0,
                       const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::DragFloat2(id, value, speed, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

inline bool DragFloat3(const char* label, float* value, float speed = 1.0f, float min = 0, float max = 0,
                       const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::DragFloat3(id, value, speed, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

inline bool DragFloat4(const char* label, float* value, float speed = 1.0f, float min = 0, float max = 0,
                       const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::DragFloat4(id, value, speed, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

inline bool DragInt(const char* label, int* value, float speed = 1.0f, int min = 0, int max = 0,
                       const char* format = "%d", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::DragInt(id, value, speed, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

inline bool DragInt2(const char* label, int* value, float speed = 1.0f, int min = 0, int max = 0,
                       const char* format = "%d", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::DragInt2(id, value, speed, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

template<typename... Args>
bool DragScalar(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::DragScalar(id, std::forward<Args>(args)...); });
}

inline bool SliderFloat(const char* label, float* value, float min, float max,
                        const char* format = "%.3f", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::SliderFloat(id, value, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

inline bool SliderInt(const char* label, int* value, int min, int max,
                        const char* format = "%d", ImGuiSliderFlags flags = 0)
{
    return Draw(label, [&](const char* id) { return ImGui::SliderInt(id, value, min, max, format, flags | ImGuiSliderFlags_AlignValueLeft); });
}

template<typename... Args>
bool ColorEdit3(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::ColorEdit3(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool ColorEdit4(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::ColorEdit4(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool InputFloat(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::InputFloat(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool InputFloat3(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::InputFloat3(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool InputInt(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::InputInt(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool InputText(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::InputText(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool InputTextWithHint(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return ImGui::InputTextWithHint(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool AssetPathField(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return widgets::AssetPathField(id, std::forward<Args>(args)...); });
}

template<typename... Args>
bool AssetPathFieldWithLoad(const char* label, Args&&... args)
{
    return Draw(label, [&](const char* id) { return widgets::AssetPathFieldWithLoad(id, std::forward<Args>(args)...); });
}

/// @note Popup は呼び出し側の選択処理まで開いているため、フィールド ID は呼び出し前に確定する。
inline bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0)
{
    if (label[0] == '#' && label[1] == '#') {
        return ImGui::BeginCombo(label, preview, flags);
    }
    widgets::PlacePropertyValue(widgets::DrawPropertyLabel(Caption(label).c_str()));
    const std::string id = std::string("###") + label;
    return ImGui::BeginCombo(id.c_str(), preview, flags);
}

template<typename... Args>
void LabelText(const char* label, const char* format, Args... args)
{
    const int length = std::snprintf(nullptr, 0, format, args...);
    if (length < 0) {
        return;
    }
    std::string text(static_cast<std::size_t>(length) + 1, '\0');
    std::snprintf(text.data(), text.size(), format, args...);
    widgets::ReadOnlyText(label, text.c_str());
}

} // namespace fbzz::editor::property
