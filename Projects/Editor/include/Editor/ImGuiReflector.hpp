// FBZZ Engine
// ImGuiReflector.hpp | fbzz::editor
// ImGui implementation of scene script reflection fields
#pragma once

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Scene/Script.hpp>
#include <imgui.h>
#include <cstdio>

namespace fbzz::editor {

struct ImGuiReflector : scene::IReflector {
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
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputText(name, buf, sizeof(buf)))
            v = buf;
    }

    void Field(const char* name, math::Quaternion& v) override
    {
        math::Vector3 euler = widgets::QuatToEulerDeg(v);
        float arr[3] = { euler.x, euler.y, euler.z };
        if (ImGui::DragFloat3(name, arr, 0.5f))
            v = widgets::EulerDegToQuat({ arr[0], arr[1], arr[2] });
    }
};

} // namespace fbzz::editor
