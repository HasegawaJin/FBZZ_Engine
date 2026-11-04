/// @file    ViewportFiberPaint.cpp
/// @brief   草の追加、表面ブラシとストローク単位の確定を Scene View へ提供する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include "ViewportCommon.hpp"
#include <Editor/Tools/FiberPaintTool.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <bit>

namespace fbzz::editor {
namespace {
std::vector<std::string> TargetIds(EditorContext& ctx, bool all)
{
    std::vector<std::string> result;
    for (const auto* go : FiberPaintTargets(ctx,all)) result.push_back(go->instanceId);
    return result;
}
uint64_t TargetSignature(EditorContext& ctx, bool all)
{
    uint64_t value = 1469598103934665603ull;
    const auto hash = [&value](uint64_t part) { value = (value^part)*1099511628211ull; };
    for (auto* go : FiberPaintTargets(ctx,all)) {
        const auto* fiber = go->GetComponent<scene::FiberComponent>();
        const auto* mesh = go->GetComponent<scene::MeshRenderer>()->mesh;
        hash(ctx.activeScene->GetComponentLifetime<scene::FiberComponent>(go->GetID()));
        hash(mesh->vertexBuffer.id); hash(mesh->vertexBuffer.gen); hash(mesh->indexBuffer.id); hash(mesh->indexBuffer.gen);
        for (unsigned char ch : fiber->m_paintPath) hash(ch);
        const auto* paint = asset::AssetManager::Get(asset::AssetManager::Load<asset::FiberPaintAsset>(fiber->m_paintPath));
        hash(paint ? paint->revision : 0);
        const auto world = go->transform.GetWorldMatrix();
        for (const auto& row : world.m) for (float scalar : row) hash(std::bit_cast<uint32_t>(scalar));
    }
    const auto camera = ctx.editorCamera->GetViewProjection();
    for (const auto& row : camera.m) for (float scalar : row) hash(std::bit_cast<uint32_t>(scalar));
    return value;
}
void ClearStroke(FiberPaintTool& tool)
{
    tool.dragging = false; tool.hasLastHit = false; tool.strokes.clear(); tool.targets.clear(); tool.transaction = {};
}
math::Ray ScreenRay(EditorContext& ctx, const ImVec2& min, const ImVec2& size, float x, float y)
{
    const float nx = (x-min.x)/size.x*2-1, ny = 1-(y-min.y)/size.y*2;
    const auto inverse = math::Matrix4::Inverse(ctx.editorCamera->GetProjectionMatrix()*ctx.editorCamera->GetViewMatrix());
    if (ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic) {
        auto p = inverse*math::Vector4{nx,ny,0,1};
        if (std::abs(p.w) > 1.0e-6f) p = p*(1/p.w);
        return {{p.x,p.y,p.z},ctx.editorCamera->GetForward()};
    }
    return math::Ray::FromNDC(nx,ny,ctx.editorCamera->m_position,inverse);
}
bool Sample(EditorContext& ctx, FiberPaintTool& tool, const math::Ray& ray, math::Vector3& point)
{
    float distance = 1000000.0f;
    uint32_t triangle = 0;
    auto* go = FiberPaintRaycast(ctx,ray.origin,ray.direction,distance,triangle);
    if (!go || std::find(tool.targets.begin(),tool.targets.end(),go->instanceId) == tool.targets.end()) {
        tool.hasLastHit = false; return false;
    }
    point = ray.origin+ray.direction*distance;
    if (tool.hasLastHit && tool.mode != 2 && (point-tool.lastHit).LengthSq() < 1.0e-12f) return true;
    renderer::FiberPaintStroke stroke;
    stroke.to = point;
    stroke.from = tool.hasLastHit && (point-tool.lastHit).Length() < tool.radius*4 ? tool.lastHit : point;
    stroke.radius = tool.radius; stroke.softness = tool.softness;
    stroke.mode = static_cast<renderer::FiberPaintMode>(tool.mode);
    stroke.target = tool.erase ? 0.0f : tool.mode == 1 ? tool.length : tool.density;
    tool.strokes.push_back(stroke);
    tool.lastHit = point; tool.lastNode = go->instanceId; tool.hasLastHit = true;
    return true;
}
} /// @note namespace
bool DrawFiberPaintTool(EditorContext& ctx, FiberPaintTool& tool, const ImVec2& min,
    const ImVec2& size, bool hovered, bool editable)
{
    SynchronizeFiberPaintDrafts();
    if (!editable || !ctx.activeScene || !ctx.editorCamera || ctx.mapEditingMode || ctx.clothPinPainting) {
        ClearStroke(tool); ctx.fiberPainting = false; return false;
    }
    const auto available = FiberPaintTargets(ctx,false,false);
    if (available.empty() && !ctx.fiberPainting) { ClearStroke(tool); return false; }
    const int frame = ImGui::GetFrameCount();
    const auto& io = ImGui::GetIO();
    if (tool.dragging && (tool.lastFrame != frame-1 || tool.transaction.owner != ctx.activeScene
        || tool.transaction.discardEpoch != AssetDirtyRegistry::DiscardEpoch() || !ctx.fiberPainting
        || tool.targets != TargetIds(ctx,tool.allRegistered)
        || tool.targetSignature != TargetSignature(ctx,tool.allRegistered)
        || (ctx.undoStack && tool.undoRevision != ctx.undoStack->GetRevision())
        || io.KeyAlt || io.KeyCtrl || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)
        || ImGui::IsKeyPressed(ImGuiKey_Escape))) ClearStroke(tool);
    tool.lastFrame = frame;
    const auto cursor = ImGui::GetCursorScreenPos();
    ImGui::PushID("FiberPaint");
    ImGui::SetCursorScreenPos({min.x+8,min.y+size.y-(ctx.fiberPainting ? 196.0f : 58.0f)});
    ImGui::BeginGroup();
    ImGui::BeginDisabled(tool.dragging);
    ImGui::SetNextItemWidth(115);
    ImGui::Combo("##preset",&tool.preset,"Short lawn\0Meadow\0Tall grass\0");
    ImGui::SameLine();
    ImGui::BeginDisabled(available.empty() || ctx.projectRoot.empty());
    if (ImGui::SmallButton("Add Grass")) {
        OpArgs args; args.Set("preset",tool.preset); args.Set("painted",tool.startEmpty);
        const auto result = InvokeOperator(ctx,"fiber.add",args); tool.diagnostic = result.message;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::Checkbox("Start empty",&tool.startEmpty);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ctx.fiberPainting && FiberPaintTargets(ctx,false).empty());
    if (ImGui::SmallButton(ctx.fiberPainting ? "Finish" : "Paint Grass")) {
        const auto result = InvokeOperator(ctx,"fiber.paint_mode"); tool.diagnostic = result.message;
        if (ctx.fiberPainting && ctx.terrainTool) ctx.terrainTool->SetActive(false);
    }
    ImGui::EndDisabled();
    if (ctx.fiberPainting) {
        ImGui::SetNextItemWidth(120);
        ImGui::Combo("Brush",&tool.mode,"Density\0Height\0Smooth\0");
        ImGui::SameLine(); ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("Size (m)",&tool.radius,0.05f,0.1f,32,"%.2f",ImGuiSliderFlags_AlwaysClamp);
        if (tool.mode != 2) {
            ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat(tool.mode == 1 ? "Height" : "Density",tool.mode == 1 ? &tool.length : &tool.density,0,1,"%.2f");
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(100); ImGui::SliderFloat("Softness",&tool.softness,0,1,"%.2f");
        ImGui::Checkbox("All registered meshes",&tool.allRegistered);
        ImGui::SameLine(); ImGui::Checkbox("Visible surface only",&tool.visibleOnly);
        for (const auto& entry : {std::pair<const char*,float>{"Fill",1.0f},{"Clear",0.0f}}) {
            if (ImGui::SmallButton(entry.first)) {
                OpArgs args; args.Set("density",entry.second); args.Set("all",tool.allRegistered);
                const auto result = InvokeOperator(ctx,"fiber.fill",args); tool.diagnostic = result.message;
            }
            ImGui::SameLine();
        }
        ImGui::TextUnformatted("Drag: paint | Shift: erase | [ ]: size | Esc: cancel | Release to apply");
    }
    ImGui::EndDisabled();
    uint64_t roots = 0, chunks = 0; bool failed = false;
    for (auto* go : FiberPaintTargets(ctx,tool.allRegistered)) {
        const auto* fiber = go->GetComponent<scene::FiberComponent>();
        roots += fiber->m_placementRootCount; chunks += fiber->m_placementChunkCount; failed = failed || fiber->m_placementFailed;
    }
    ImGui::Text("Grass: %llu blades / %llu chunks / %.1f MiB GPU",static_cast<unsigned long long>(roots),
        static_cast<unsigned long long>(chunks),static_cast<double>(roots*64)/1048576.0);
    if (failed) ImGui::TextColored({1,0.65f,0.25f,1},"Check mesh shape, paint asset or blade budget in Fiber settings.");
    if (!tool.diagnostic.empty()) ImGui::TextUnformatted(tool.diagnostic.c_str());
    ImGui::EndGroup();
    const bool controlsHovered = ImGui::IsItemHovered();
    ImGui::PopID(); ImGui::SetCursorScreenPos(cursor);
    if (!ctx.fiberPainting) { ClearStroke(tool); return controlsHovered; }
    const bool canPaint = hovered && !controlsHovered && !io.KeyAlt && !io.KeyCtrl
        && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)
        && !ImGui::IsDragDropActive() && !ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    const auto mouse = ImGui::GetMousePos();
    if (canPaint) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) tool.radius = std::max(0.1f,tool.radius*0.8f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) tool.radius = std::min(32.0f,tool.radius*1.25f);
    }
    if (canPaint && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ClearStroke(tool); tool.dragging = true; tool.erase = io.KeyShift;
        tool.targets = TargetIds(ctx,tool.allRegistered);
        tool.targetSignature = TargetSignature(ctx,tool.allRegistered);
        tool.transaction.owner = ctx.activeScene; tool.transaction.discardEpoch = AssetDirtyRegistry::DiscardEpoch();
        tool.undoRevision = ctx.undoStack ? ctx.undoStack->GetRevision() : 0;
        tool.lastMouseX = mouse.x; tool.lastMouseY = mouse.y; tool.diagnostic.clear();
    }
    if (tool.dragging && canPaint && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
        const float dx = mouse.x-tool.lastMouseX, dy = mouse.y-tool.lastMouseY;
        const int steps = static_cast<int>(std::clamp(std::ceil(std::sqrt(dx*dx+dy*dy)/8.0f),1.0f,128.0f));
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i)/static_cast<float>(steps);
            math::Vector3 point;
            (void)Sample(ctx,tool,ScreenRay(ctx,min,size,tool.lastMouseX+dx*t,tool.lastMouseY+dy*t),point);
        }
        tool.lastMouseX = mouse.x; tool.lastMouseY = mouse.y;
        if (tool.strokes.size() > 4096) { ClearStroke(tool); tool.diagnostic = "Stroke is too long; paint in shorter strokes."; }
    } else if (tool.dragging && !canPaint) tool.hasLastHit = false;
    if (canPaint) {
        const auto ray = ScreenRay(ctx,min,size,mouse.x,mouse.y);
        float distance = 1000000; uint32_t triangle = 0;
        auto* hit = FiberPaintRaycast(ctx,ray.origin,ray.direction,distance,triangle);
        if (hit) {
            const auto point = ray.origin+ray.direction*distance;
            const auto color = (tool.dragging ? tool.erase : io.KeyShift) ? IM_COL32(255,110,100,230) : IM_COL32(90,240,130,230);
            auto* draw = ImGui::GetWindowDrawList(); draw->PushClipRect(min,{min.x+size.x,min.y+size.y},true);
            ImVec2 previous{}; bool hasPrevious = false;
            for (int i = 0; i <= 48; ++i) {
                const float angle = static_cast<float>(i)*6.283185307179586f/48;
                const auto world = point+math::Vector3{std::cos(angle)*tool.radius,0,std::sin(angle)*tool.radius};
                ImVec2 screen;
                if (WorldToScreen(world,ctx,min,size,screen)) { if (hasPrevious) draw->AddLine(previous,screen,color,2); previous = screen; hasPrevious = true; }
                else hasPrevious = false;
            }
            for (const auto& sample : tool.strokes) {
                ImVec2 a,b;
                if (WorldToScreen(sample.from,ctx,min,size,a) && WorldToScreen(sample.to,ctx,min,size,b)) draw->AddLine(a,b,color,3);
            }
            draw->PopClipRect();
        }
    }
    if (tool.dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!tool.strokes.empty()) {
            std::string reason;
            if (PrepareFiberPaintEdits(ctx,tool.strokes,tool.allRegistered,tool.visibleOnly,tool.transaction.edits,reason)
                && ApplyFiberPaintPreview(ctx,tool.transaction.edits,reason)) {
                ctx.fiberPaintPending = &tool.transaction;
                const auto result = InvokeOperator(ctx,"fiber.commit_stroke");
                ctx.fiberPaintPending = nullptr;
                if (!result.ok) { CancelFiberPaintPreview(ctx,tool.transaction); tool.diagnostic = result.message; }
            } else tool.diagnostic = reason;
        }
        ClearStroke(tool);
    } else if (tool.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ClearStroke(tool);
    return controlsHovered;
}
} /// @note namespace fbzz::editor
