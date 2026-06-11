// FBZZ Engine
// ImGuiReflector.hpp | fbzz::editor
// ImGui implementation of scene script reflection fields
#pragma once

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Input/KeyCode.hpp>
#include <Engine/Scene/Script.hpp>
#include <imgui.h>
#include <cstdio>
#include <functional>
#include <string>

namespace fbzz::editor {

struct ImGuiReflector : scene::IReflector {
    // GO 名解決コールバック。InspectorCore から activeScene を渡して設定する。
    std::function<std::string(scene::EntityID)> m_goNameResolver;

    // ── 基本型 ───────────────────────────────────────────────────────────────

    void Field(const char* name, float& v) override
    {
        ImGui::DragFloat(name, &v, 0.1f);
    }

    void Field(const char* name, int& v) override
    {
        ImGui::DragInt(name, &v);
    }

    void Field(const char* name, bool& v) override
    {
        ImGui::Checkbox(name, &v);
    }

    void Field(const char* name, math::Vector2& v) override
    {
        float arr[2] = { v.x, v.y };
        if (ImGui::DragFloat2(name, arr, 0.1f))
            v = { arr[0], arr[1] };
    }

    void Field(const char* name, math::Vector3& v) override
    {
        float arr[3] = { v.x, v.y, v.z };
        if (ImGui::DragFloat3(name, arr, 0.1f))
            v = { arr[0], arr[1], arr[2] };
    }

    void Field(const char* name, math::Vector4& v) override
    {
        float arr[4] = { v.x, v.y, v.z, v.w };
        if (ImGui::ColorEdit4(name, arr))
            v = { arr[0], arr[1], arr[2], arr[3] };
    }

    void Field(const char* name, std::string& v) override
    {
        std::string buf = v;
        buf.resize(buf.size() + 128);
        if (ImGui::InputText(name, buf.data(), buf.capacity()))
            v = buf.data();
    }

    void Field(const char* name, math::Quaternion& v) override
    {
        widgets::DragQuatEuler3(name, v, 0.5f);
    }

    // ── 参照型 ───────────────────────────────────────────────────────────────

    void Field(const char* name, scene::EntityID& v) override
    {
        ImGui::PushID(name);

        // GO 名を解決して表示ラベルを作る
        std::string goName = "(None)";
        if (v.IsValid() && m_goNameResolver)
            goName = m_goNameResolver(v);
        else if (v.IsValid())
            goName = "(ID:" + std::to_string(v.index) + ")";

        const float btnW = ImGui::GetContentRegionAvail().x;
        ImGui::Button(goName.c_str(), { btnW, 0.0f });

        if (ImGui::BeginDragDropTarget()) {
            if (auto* payload = ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY")) {
                scene::EntityID dropped;
                std::memcpy(&dropped, payload->Data, sizeof(dropped));
                v = dropped;
            }
            ImGui::EndDragDropTarget();
        }

        // ×ボタンで参照クリア
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            v = scene::EntityID::INVALID;

        ImGui::SameLine();
        ImGui::TextUnformatted(name);

        ImGui::PopID();
    }

    void Field(const char* name, scene::PrefabRef& v) override
    {
        ImGui::PushID(name);

        const auto  slash   = v.path.find_last_of("/\\");
        const char* display = v.path.empty() ? "(None)"
                            : (slash != std::string::npos ? v.path.c_str() + slash + 1
                                                          : v.path.c_str());
        ImGui::Button(display, { ImGui::GetContentRegionAvail().x, 0.0f });

        if (ImGui::BeginDragDropTarget()) {
            if (auto* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                v.path.assign(static_cast<const char*>(payload->Data),
                              static_cast<size_t>(payload->DataSize - 1));
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            v.path.clear();

        ImGui::SameLine();
        ImGui::TextUnformatted(name);

        ImGui::PopID();
    }

    // ── ヒント付きフィールド ─────────────────────────────────────────────────

    void FloatRange(const char* name, float& v, float min, float max) override
    {
        ImGui::SliderFloat(name, &v, min, max);
    }

    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        if (labels.empty()) return;
        const char* current = (v >= 0 && v < (int)labels.size()) ? labels[v] : "??";
        if (ImGui::BeginCombo(name, current)) {
            for (int i = 0; i < (int)labels.size(); ++i) {
                const bool selected = (v == i);
                if (ImGui::Selectable(labels[i], selected))
                    v = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    void Header(const char* label)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(label);
    }

    void Field(const char* name, input::KeyCode& v) override
    {
        int raw = static_cast<int>(v);
        KeyCodeField(name, raw);
        v = static_cast<input::KeyCode>(raw);
    }

    void KeyCodeField(const char* name, int& v)
    {
        ImGui::PushID(name);

        static int* s_listeningPtr = nullptr;
        const bool isListening = (s_listeningPtr == &v);

        if (isListening) {
            ImGui::Button("Press any key...", { -1.0f, 0.0f });
            // Escape でキャンセル
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                s_listeningPtr = nullptr;
            } else {
                // ImGuiKey → Win32 VK の主要対応表でスキャン
                static constexpr struct { ImGuiKey imk; int vk; } kMap[] = {
                    {ImGuiKey_A,'A'},{ImGuiKey_B,'B'},{ImGuiKey_C,'C'},{ImGuiKey_D,'D'},
                    {ImGuiKey_E,'E'},{ImGuiKey_F,'F'},{ImGuiKey_G,'G'},{ImGuiKey_H,'H'},
                    {ImGuiKey_I,'I'},{ImGuiKey_J,'J'},{ImGuiKey_K,'K'},{ImGuiKey_L,'L'},
                    {ImGuiKey_M,'M'},{ImGuiKey_N,'N'},{ImGuiKey_O,'O'},{ImGuiKey_P,'P'},
                    {ImGuiKey_Q,'Q'},{ImGuiKey_R,'R'},{ImGuiKey_S,'S'},{ImGuiKey_T,'T'},
                    {ImGuiKey_U,'U'},{ImGuiKey_V,'V'},{ImGuiKey_W,'W'},{ImGuiKey_X,'X'},
                    {ImGuiKey_Y,'Y'},{ImGuiKey_Z,'Z'},
                    {ImGuiKey_0,'0'},{ImGuiKey_1,'1'},{ImGuiKey_2,'2'},{ImGuiKey_3,'3'},
                    {ImGuiKey_4,'4'},{ImGuiKey_5,'5'},{ImGuiKey_6,'6'},{ImGuiKey_7,'7'},
                    {ImGuiKey_8,'8'},{ImGuiKey_9,'9'},
                    {ImGuiKey_Space,   0x20}, // VK_SPACE
                    {ImGuiKey_Enter,   0x0D}, // VK_RETURN
                    {ImGuiKey_Backspace,0x08},// VK_BACK
                    {ImGuiKey_LeftShift,0x10},{ImGuiKey_RightShift,0x10},   // VK_SHIFT
                    {ImGuiKey_LeftCtrl, 0x11},{ImGuiKey_RightCtrl, 0x11},   // VK_CONTROL
                    {ImGuiKey_LeftAlt,  0x12},{ImGuiKey_RightAlt,  0x12},   // VK_MENU
                    {ImGuiKey_LeftArrow,0x25},{ImGuiKey_RightArrow,0x27},
                    {ImGuiKey_UpArrow,  0x26},{ImGuiKey_DownArrow, 0x28},
                    {ImGuiKey_F1,0x70},{ImGuiKey_F2,0x71},{ImGuiKey_F3,0x72},{ImGuiKey_F4,0x73},
                    {ImGuiKey_F5,0x74},{ImGuiKey_F6,0x75},{ImGuiKey_F7,0x76},{ImGuiKey_F8,0x77},
                    {ImGuiKey_F9,0x78},{ImGuiKey_F10,0x79},{ImGuiKey_F11,0x7A},{ImGuiKey_F12,0x7B},
                };
                for (auto& e : kMap) {
                    if (ImGui::IsKeyPressed(e.imk, false)) {
                        v = e.vk;
                        s_listeningPtr = nullptr;
                        break;
                    }
                }
            }
        } else {
            const char* keyName = KeyCodeName(v);
            char label[128];
            std::snprintf(label, sizeof(label), "[%s]  %s", keyName, name);
            if (ImGui::Button(label, { -1.0f, 0.0f }))
                s_listeningPtr = &v;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("クリックでキー入力待ち (現在: %s / VK %d)", keyName, v);
        }

        ImGui::PopID();
    }

    void FieldWithTooltip(const char* name, float& v, const char* tooltip)
    {
        ImGui::DragFloat(name, &v, 0.1f);
        if (ImGui::IsItemHovered() && tooltip && tooltip[0])
            ImGui::SetTooltip("%s", tooltip);
    }

    void Label(const char* name, const std::string& v)
    {
        ImGui::LabelText(name, "%s", v.c_str());
    }

    void Label(const char* name, float v)
    {
        ImGui::LabelText(name, "%.3f", v);
    }

    void Label(const char* name, int v)
    {
        ImGui::LabelText(name, "%d", v);
    }

    void Separator()
    {
        ImGui::Separator();
    }

private:
    // Win32 VK コード → 表示名
    static const char* KeyCodeName(int vk)
    {
        switch (vk) {
        case 'A': return "A"; case 'B': return "B"; case 'C': return "C";
        case 'D': return "D"; case 'E': return "E"; case 'F': return "F";
        case 'G': return "G"; case 'H': return "H"; case 'I': return "I";
        case 'J': return "J"; case 'K': return "K"; case 'L': return "L";
        case 'M': return "M"; case 'N': return "N"; case 'O': return "O";
        case 'P': return "P"; case 'Q': return "Q"; case 'R': return "R";
        case 'S': return "S"; case 'T': return "T"; case 'U': return "U";
        case 'V': return "V"; case 'W': return "W"; case 'X': return "X";
        case 'Y': return "Y"; case 'Z': return "Z";
        case '0': return "0"; case '1': return "1"; case '2': return "2";
        case '3': return "3"; case '4': return "4"; case '5': return "5";
        case '6': return "6"; case '7': return "7"; case '8': return "8";
        case '9': return "9";
        case 0x20: return "Space";
        case 0x0D: return "Enter";
        case 0x08: return "Backspace";
        case 0x1B: return "Escape";
        case 0x10: return "Shift";
        case 0x11: return "Ctrl";
        case 0x12: return "Alt";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x70: return "F1";  case 0x71: return "F2";  case 0x72: return "F3";
        case 0x73: return "F4";  case 0x74: return "F5";  case 0x75: return "F6";
        case 0x76: return "F7";  case 0x77: return "F8";  case 0x78: return "F9";
        case 0x79: return "F10"; case 0x7A: return "F11"; case 0x7B: return "F12";
        default:   return "?";
        }
    }
};

} // namespace fbzz::editor
