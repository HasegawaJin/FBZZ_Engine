/// @file    ViewportClothPins.cpp
/// @brief   布質点の透過表示とストローク単位の固定点ペイント。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "ViewportCommon.hpp"
#include <Editor/Tools/ClothPinBrush.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>

namespace fbzz::editor {
void DrawClothPinBrush(EditorContext& ctx, ClothPinBrush& brush, const ImVec2& min, const ImVec2& size, bool hovered, bool editable)
{
    auto* go = ctx.GetSelectedGO();
    auto* cloth = go ? go->GetComponent<scene::ClothComponent>() : nullptr;
    if (!editable || !cloth || !ctx.editorCamera || ctx.mapEditingMode || (ctx.terrainTool && ctx.terrainTool->IsActive())) {
        ctx.clothPinPainting = false;
        brush.dragging = false;
        return;
    }
    const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({min.x + 8, min.y + size.y - 64});
    ImGui::BeginGroup();
    if (ImGui::SmallButton(ctx.clothPinPainting ? "Finish Cloth Pins" : "Paint Cloth Pins"))
        InvokeOperator(ctx, "cloth.paint_mode");
    if (ctx.clothPinPainting) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Radius (px)", &brush.radius, 4.0f, 100.0f, "%.0f");
        ImGui::TextUnformatted("Through selection | Drag: pin | Shift+drag: release | Gold: pinned");
    }
    ImGui::EndGroup();
    const bool controlsHovered = ImGui::IsItemHovered();
    ImGui::SetCursorScreenPos(savedCursor);
    const int frame = ImGui::GetFrameCount();
    auto& state = cloth->runtime;
    if (!ctx.clothPinPainting || !state.initialized || state.failed || !cloth->enabled) {
        brush.dragging = false;
        return;
    }
    const auto& io = ImGui::GetIO();
    if (brush.dragging && (brush.lastFrame != frame - 1 || brush.scene != ctx.activeScene
        || brush.target != go->instanceId || brush.assetPath != cloth->clothAssetPath
        || brush.revision != state.assetRevision || brush.shape != state.shape
        || brush.beforeOverride != cloth->overridePins || brush.beforePins != cloth->pinnedParticles
        || brush.touched.size() != state.restLocal.size() || io.KeyAlt || io.KeyCtrl
        || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)
        || ImGui::IsKeyPressed(ImGuiKey_Escape)))
        brush.dragging = false;
    brush.lastFrame = frame;
    const bool canPaint = hovered && !controlsHovered && !ImGui::IsAnyItemActive()
        && !io.KeyAlt && !io.KeyCtrl && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)
        && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const ImVec2 mouse = ImGui::GetMousePos();
    if (canPaint && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        brush.dragging = true;
        brush.remove = io.KeyShift;
        brush.scene = ctx.activeScene;
        brush.target = go->instanceId;
        brush.assetPath = cloth->clothAssetPath;
        brush.revision = state.assetRevision;
        brush.shape = state.shape;
        brush.beforeOverride = cloth->overridePins;
        brush.beforePins = cloth->pinnedParticles;
        brush.touched.assign(state.restLocal.size(), false);
        brush.lastX = mouse.x;
        brush.lastY = mouse.y;
    }
    std::vector<bool> pinned(state.restLocal.size(), false);
    if (cloth->overridePins) {
        for (int id : cloth->pinnedParticles) if (id >= 0 && static_cast<size_t>(id) < pinned.size()) pinned[id] = true;
    } else {
        for (uint32_t id : state.pins) if (id < pinned.size()) pinned[id] = true;
    }
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(min, {min.x + size.x, min.y + size.y}, true);
    const auto vp = ctx.editorCamera->GetProjectionMatrix() * ctx.editorCamera->GetViewMatrix();
    for (size_t i = 0; i < state.restLocal.size(); ++i) {
        const auto& local = state.restLocal[i];
        const auto& t = go->transform;
        const auto world = t.worldPosition + t.worldRotation * math::Vector3{local.x*t.worldScale.x, local.y*t.worldScale.y, local.z*t.worldScale.z};
        const auto clip = vp * math::Vector4{world.x, world.y, world.z, 1.0f};
        if (clip.w <= 0.0f || clip.z < 0.0f || clip.z > clip.w) continue;
        const ImVec2 p{min.x + (clip.x/clip.w + 1)*size.x*0.5f, min.y + (1-clip.y/clip.w)*size.y*0.5f};
        if (p.x < min.x || p.y < min.y || p.x > min.x + size.x || p.y > min.y + size.y) continue;
        if (brush.dragging && canPaint) {
            /// @note マウスの前回位置からの線分を太らせ、速いドラッグでもフレーム間の塗り残しを防ぐ。
            const float dx = mouse.x - brush.lastX, dy = mouse.y - brush.lastY;
            const float lengthSq = dx*dx + dy*dy;
            const float u = lengthSq > 0 ? std::clamp(((p.x-brush.lastX)*dx + (p.y-brush.lastY)*dy)/lengthSq, 0.0f, 1.0f) : 0;
            const float x = p.x - (brush.lastX + u*dx), y = p.y - (brush.lastY + u*dy);
            if (x*x + y*y <= brush.radius*brush.radius) brush.touched[i] = true;
        }
        const bool touched = brush.dragging && brush.touched[i];
        const bool fixed = touched ? !brush.remove : pinned[i];
        draw->AddCircleFilled(p, touched ? 4.0f : 3.0f, fixed ? IM_COL32(255,190,50,255) : IM_COL32(70,210,255,210));
    }
    if (canPaint) draw->AddCircle(mouse, brush.radius, IM_COL32(240,240,240,220), 40, 1.5f);
    draw->PopClipRect();
    if (brush.dragging) {
        brush.lastX = mouse.x;
        brush.lastY = mouse.y;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (canPaint) {
                std::string ids;
                for (size_t i = 0; i < brush.touched.size(); ++i) if (brush.touched[i]) ids += std::to_string(i) + " ";
                OpArgs args;
                args.Set("node", brush.target);
                args.Set("particles", ids);
                args.Set("pin", !brush.remove);
                const auto result = InvokeOperator(ctx, "cloth.paint_pins", args);
                if (!result.ok) FBZZ_LOG_WARN("Cloth brush: %s", result.message.c_str());
            }
            brush.dragging = false;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) brush.dragging = false;
    }
}
}
