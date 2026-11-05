/// @file    TerrainPaintLibrary.cpp
/// @brief   専用 Paint 素材を検索・選択し、同じ材質プレビューで作成・編集する。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "TerrainPaintLibrary.hpp"
#include "TerrainPaintEditing.hpp"
#include "TerrainTool.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Panels/MaterialPreviewCore.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace fbzz::editor {
namespace {

struct PaintForm {
    asset::TerrainPaintAsset value;
    std::vector<asset::InstanceMeshPart> parts{{"", "", 0}};
    std::vector<float> heights{0.0f};
    std::string path, expected, status, importSource;
    bool create = true;
    asset::AssetHandle<asset::TerrainPaintAsset> handle;
};

asset::TerrainPaintAsset* Resolve(const std::string& path)
{
    return asset::AssetManager::Get(asset::AssetManager::Load<asset::TerrainPaintAsset>(path));
}

void LoadForm(PaintForm& form, std::string path, const asset::TerrainPaintAsset& source)
{
    form = {};
    form.value = source; form.path = path; form.create = false;
    form.parts.assign(source.definition.Parts().begin(), source.definition.Parts().end());
    form.heights.assign(source.definition.LodHeights().begin(), source.definition.LodHeights().end());
    form.expected = TerrainPaintAssetFingerprint(source);
    form.handle = asset::AssetManager::Load<asset::TerrainPaintAsset>(path);
}

void DrawStatus(const std::string& status)
{
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}

bool DrawForm(EditorContext& ctx, PaintForm& form, bool modal)
{
    if (!form.create) {
        const auto* live = asset::AssetManager::Get(form.handle);
        auto draft = form.value;
        if (live && TerrainPaintAssetFingerprint(*live) != form.expected
            && draft.definition.Replace(form.parts, form.heights, {}) && TerrainPaintAssetFingerprint(draft) == form.expected)
            LoadForm(form, form.path, *live);
    }
    widgets::InputString("Name", form.value.name);
    constexpr const char* categories[]{"Grass", "Tree", "Rock", "Other"};
    int category = static_cast<int>(form.value.category);
    if (ImGui::Combo("Category", &category, categories, IM_ARRAYSIZE(categories)))
        form.value.category = static_cast<asset::TerrainPaintCategory>(category);
    if (form.create) widgets::InputString("Save Path", form.path);
    if (ImGui::CollapsingHeader("Import Existing Definition")) {
        widgets::AssetPathField("Source", form.importSource, ".fzdata,.terrainpaint,.meshinstances", ctx.projectRoot);
        if (ImGui::Button("Copy Model / Materials / LOD")) {
            if (const auto* source = asset::ResolveMeshInstanceDefinition(form.importSource)) {
                form.parts.assign(source->Parts().begin(), source->Parts().end());
                form.heights.assign(source->LodHeights().begin(), source->LodHeights().end());
            } else form.status = "The selected definition could not be loaded.";
        }
    }
    ImGui::SeparatorText("Model / Materials");
    for (size_t i = 0; i < form.parts.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("Part %zu", i + 1);
        widgets::AssetPathField("Mesh", form.parts[i].meshPath, ".fbx,.gltf,.glb,.obj,.mesh,.fzasset", ctx.projectRoot);
        widgets::AssetPathField("Material", form.parts[i].materialPath, ".mat", ctx.projectRoot);
        int lod = static_cast<int>(form.parts[i].lod);
        if (ImGui::SliderInt("LOD", &lod, 0, static_cast<int>(form.heights.size()) - 1)) form.parts[i].lod = static_cast<uint32_t>(lod);
        if (form.parts.size() > 1 && ImGui::SmallButton("Remove Part")) {
            form.parts.erase(form.parts.begin() + i);
            ImGui::PopID(); break;
        }
        ImGui::PopID();
    }
    if (form.parts.size() < 64 && ImGui::Button("Add Mesh Part")) form.parts.push_back({"", "", 0});
    if (ImGui::CollapsingHeader("LOD Thresholds")) {
        for (size_t i = 0; i < form.heights.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::DragFloat("Screen Height", &form.heights[i], 0.01f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::PopID();
        }
        if (form.heights.size() < 4 && ImGui::SmallButton("Add LOD")) {
            if (form.heights.back() == 0.0f) form.heights.back() = form.heights.size() == 1 ? 0.2f : form.heights[form.heights.size() - 2] * 0.5f;
            form.heights.push_back(0.0f);
        }
        if (form.heights.size() > 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove Last LOD")) {
                form.heights.pop_back();
                form.heights.back() = 0.0f;
                for (auto& part : form.parts) part.lod = std::min(part.lod, static_cast<uint32_t>(form.heights.size() - 1));
            }
        }
    }
    ImGui::SeparatorText("Placement Defaults");
    auto& p = form.value.placement;
    ImGui::DragFloat("Spacing [m]", &p.spacing, 0.1f, 0.25f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Density", &p.density, 0.0f, 1.0f);
    ImGui::DragFloatRange2("Scale Range", &p.minScale, &p.maxScale, 0.01f, 0.01f, 100.0f);
    ImGui::DragFloatRange2("Yaw [deg]", &p.minYaw, &p.maxYaw, 1.0f, -360.0f, 360.0f);
    ImGui::DragFloatRange2("Slope [deg]", &p.minSlope, &p.maxSlope, 0.5f, 0.0f, 90.0f);
    ImGui::Checkbox("Align to Surface", &p.alignToNormal);
    ImGui::Checkbox("Face Downslope", &p.alignDownslope);
    if (p.alignDownslope) ImGui::SliderFloat("Downslope Jitter [deg]", &p.downslopeJitter, 0.0f, 180.0f);
    ImGui::DragFloat("Embed Depth [m]", &p.embedDepth, 0.01f, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    asset::TerrainPaintAsset candidate = form.value;
    const bool valid = candidate.definition.Replace(form.parts, form.heights, {}) && asset::ValidateTerrainPaintAsset(candidate);
    if (!valid) ImGui::TextWrapped("Assign every mesh and material. Each LOD needs a part and descending screen-height thresholds.");
    bool saved = false;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(form.create ? "Create Paint Asset" : "Save Paint Asset")) {
        if (form.create && (form.path.empty() || form.path == "Assets/Terrain/Paint/New Paint.terrainpaint"))
            form.path = "Assets/Terrain/Paint/" + SanitizeMaterialFileName(candidate.name, 0) + ".terrainpaint";
        auto args = TerrainPaintAssetArguments(form.path, candidate);
        args.Set("expected", form.expected);
        const auto result = InvokeOperator(ctx, form.create ? "terrain.paint_asset.create" : "terrain.paint_asset.save", args);
        form.status = result.message;
        if (result.ok) { form.value = candidate; form.expected = TerrainPaintAssetFingerprint(candidate); saved = true; }
    }
    ImGui::EndDisabled();
    if (!form.create) {
        ImGui::SameLine();
        if (ImGui::Button("Reload Paint Asset")) {
            asset::TerrainPaintAsset source;
            if (asset::LoadTerrainPaintAsset(asset::AssetManager::ResolveAssetPath(form.path), source)) LoadForm(form, form.path, source);
        }
    }
    if (modal) { ImGui::SameLine(); if (ImGui::Button("Close")) ImGui::CloseCurrentPopup(); }
    DrawStatus(form.status);
    return saved;
}
} /// @note namespace

struct TerrainPaintLibrary::Impl {
    struct Thumbnail {
        renderer::ResourceHandle<renderer::RenderTargetTag> target;
        std::string signature;
        int assetGeneration = -1;
        bool rendered = false;
        bool attempted = false;
        int usedFrame = -1;
    };
    std::unordered_map<std::string, Thumbnail> thumbnails;
    std::unordered_map<std::string, asset::AssetHandle<asset::TerrainPaintAsset>> sources;
    renderer::ResourceManager* resources = nullptr;
    uint64_t resourceId = 0, reset = 0;
    std::string project;
    std::string search, active, inspectorPath;
    int category = 0;
    int renderedThisFrame = 0;
    int frame = -1;
    PaintForm form, inspector;
    bool openForm = false;
    matpreview::Orbit orbit;

    const asset::TerrainPaintAsset* Source(const std::string& path)
    {
        auto& handle = sources[path];
        if (const auto* value = asset::AssetManager::Get(handle)) return value;
        handle = asset::AssetManager::Load<asset::TerrainPaintAsset>(path);
        return asset::AssetManager::Get(handle);
    }

    void Release()
    {
        if (resources && resources->GetInstanceId() == resourceId && resources->GetResetVersion() == reset)
            for (auto& [path, thumbnail] : thumbnails) if (thumbnail.target.IsValid()) resources->Release(thumbnail.target);
        thumbnails.clear(); sources.clear();
    }

    void Begin(EditorContext& ctx)
    {
        const auto id = ctx.resources ? ctx.resources->GetInstanceId() : 0;
        const auto version = ctx.resources ? ctx.resources->GetResetVersion() : 0;
        if (project != ctx.projectRoot || resources != ctx.resources || id != resourceId || version != reset) {
            Release(); project = ctx.projectRoot; resources = ctx.resources; resourceId = id; reset = version;
            active.clear(); inspectorPath.clear();
        }
        if (frame != ImGui::GetFrameCount()) { frame = ImGui::GetFrameCount(); renderedThisFrame = 0; }
    }

    void* Preview(EditorContext& ctx, const std::string& key, const asset::TerrainPaintAsset& asset, bool detail)
    {
        if (!ctx.renderer || !ctx.resources || !ctx.imguiRenderer) return nullptr;
        if (thumbnails.size() >= 48 && !thumbnails.contains(key)) {
            auto oldest = std::min_element(thumbnails.begin(), thumbnails.end(), [](const auto& a, const auto& b) { return a.second.usedFrame < b.second.usedFrame; });
            if (oldest == thumbnails.end() || oldest->second.usedFrame >= frame - 1) return nullptr;
            if (oldest->second.target.IsValid()) resources->Release(oldest->second.target);
            thumbnails.erase(oldest);
        }
        auto& thumbnail = thumbnails[key];
        thumbnail.usedFrame = frame;
        const auto signature = TerrainPaintAssetFingerprint(asset) + (detail ? std::to_string(orbit.yaw) + "/" + std::to_string(orbit.pitch) : "");
        const int generation = asset::AssetManager::GetAssetGeneration();
        if (thumbnail.signature != signature || thumbnail.assetGeneration != generation) { thumbnail.rendered = false; thumbnail.attempted = false; }
        if (!thumbnail.target.IsValid()) thumbnail.target = resources->CreateRenderTarget(detail ? 256 : 128, detail ? 256 : 128, renderer::CameraDepthTargetDesc(1));
        if (!thumbnail.rendered && !thumbnail.attempted && renderedThisFrame < 2 && thumbnail.target.IsValid()) {
            ++renderedThisFrame;
            thumbnail.attempted = true;
            uint32_t lod = 4;
            for (const auto& part : asset.definition.Parts()) lod = std::min(lod, part.lod);
            struct Part { renderer::Mesh* mesh; const asset::InstanceMeshPart* source; };
            std::vector<Part> parts;
            math::Vector3 center{};
            for (const auto& part : asset.definition.Parts()) {
                if (part.lod != lod) continue;
                auto* mesh = scene::ResolveMeshPath(part.meshPath, *resources);
                if (!mesh || mesh->isSkinned) continue;
                parts.push_back({mesh, &part}); center += mesh->boundsCenter;
            }
            if (!parts.empty()) {
                center = center / static_cast<float>(parts.size());
                float radius = 0.01f;
                for (const auto& part : parts) radius = std::max(radius, (part.mesh->boundsCenter - center).Length() + part.mesh->boundsRadius);
                bool clear = true, valid = true;
                for (const auto& part : parts) {
                    const auto live = asset::ReadMaterialAuthoring(asset::AssetManager::Load<asset::MaterialAsset>(part.source->materialPath));
                    if (!live) { valid = false; break; }
                    matpreview::GpuData gpu;
                    const auto material = std::make_shared<const asset::MaterialAssetAuthoring>(live->Data());
                    if (matpreview::DetectFlavor(*material) != matpreview::Flavor::Surface || !matpreview::BuildGpuData(gpu, *material, *resources, ctx.projectRoot)) {
                        matpreview::ResetGpuData(gpu, resources); valid = false; break;
                    }
                    matpreview::RenderDesc desc;
                    desc.target = thumbnail.target; desc.mesh = part.mesh; desc.material = material; desc.gpu = &gpu;
                    desc.clear = clear; desc.overrideCenter = center; desc.overrideRadius = radius;
                    if (detail) desc.orbit = orbit;
                    valid = matpreview::Render(*ctx.renderer, *resources, desc) && valid;
                    matpreview::ResetGpuData(gpu, resources);
                    clear = false;
                }
                thumbnail.rendered = valid;
            }
            thumbnail.signature = signature; thumbnail.assetGeneration = generation;
        }
        return thumbnail.rendered ? ctx.imguiRenderer->GetImTextureID(thumbnail.target, *resources, 0) : nullptr;
    }
};

TerrainPaintLibrary::TerrainPaintLibrary() : m_impl(std::make_unique<Impl>()) {}
TerrainPaintLibrary::~TerrainPaintLibrary() { m_impl->Release(); }

void TerrainPaintLibrary::OpenCreate(const std::string& directory)
{
    auto& state = *m_impl;
    state.form = {}; state.form.value.name = "New Paint";
    state.form.path = directory + "/New Paint.terrainpaint";
    state.openForm = true;
}

bool TerrainPaintLibrary::Draw(EditorContext& ctx, std::span<const std::string> selected, std::vector<std::string>& next)
{
    auto& state = *m_impl;
    state.Begin(ctx);
    AssetSearch::SetProjectRoot(ctx.projectRoot);
    next.assign(selected.begin(), selected.end());
    bool changed = false;
    ImGui::SeparatorText("Paint Library");
    widgets::InputString("Search Paint Assets", state.search);
    constexpr const char* categories[]{"All", "Grass", "Trees", "Rocks", "Other"};
    ImGui::Combo("Filter", &state.category, categories, IM_ARRAYSIZE(categories));
    if (ImGui::Button("+ Create Paint Asset")) {
        OpenCreate("Assets/Terrain/Paint");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh Library")) { AssetSearch::Rebuild(); state.Release(); }
    const auto& entries = AssetSearch::Entries();
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 96.0f));
    size_t shown = 0;
    ImGui::BeginChild("##PaintAssetLibrary", {0.0f, 260.0f}, true);
    if (ImGui::BeginTable("##PaintAssetTiles", columns, ImGuiTableFlags_SizingStretchSame)) {
        for (const auto& entry : entries) {
            if (entry.extension != ".terrainpaint") continue;
            const auto* value = state.Source(entry.absolutePath);
            if (!value || (state.category && static_cast<int>(value->category) != state.category - 1)
                || (!state.search.empty() && !AssetSearch::Match(value->name + " " + entry.relativePath, state.search))) continue;
            ++shown;
            const auto guid = asset::AssetDatabase::TryGetGuidFromPath(entry.absolutePath);
            if (guid.empty()) continue;
            const auto reference = "guid:" + guid + "|" + entry.relativePath;
            bool checked = std::any_of(next.begin(), next.end(), [&](const auto& item) { return asset::AssetDatabase::GuidFromRef(item) == guid; });
            ImGui::TableNextColumn(); ImGui::PushID(guid.c_str());
            const auto origin = ImGui::GetCursorScreenPos();
            const float size = std::min(88.0f, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton("##PaintThumbnail", {size, size});
            if (void* texture = ImGui::IsItemVisible() ? state.Preview(ctx, entry.absolutePath, *value, false) : nullptr)
                ImGui::GetWindowDrawList()->AddImage(widgets::ToImTextureID(texture), origin, {origin.x + size, origin.y + size});
            else ImGui::GetWindowDrawList()->AddText({origin.x + 6.0f, origin.y + size * 0.4f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "Preview");
            if (ImGui::IsItemClicked()) {
                if (!ImGui::GetIO().KeyCtrl) { next = {reference}; checked = true; }
                else if (checked) { std::erase_if(next, [&](const auto& item) { return asset::AssetDatabase::GuidFromRef(item) == guid; }); checked = false; }
                else if (next.size() < 16) { next.push_back(reference); checked = true; }
                state.active = entry.absolutePath; changed = true;
            }
            bool choose = checked;
            if (ImGui::Checkbox(value->name.c_str(), &choose)) {
                if (choose && next.size() < 16) next.push_back(reference);
                else if (!choose) std::erase_if(next, [&](const auto& item) { return asset::AssetDatabase::GuidFromRef(item) == guid; });
                state.active = entry.absolutePath; changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.relativePath.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (shown == 0) ImGui::TextWrapped("No matching .terrainpaint assets. Create one from a mesh and material, or copy an existing Instance Set.");
    ImGui::EndChild();
    ImGui::TextDisabled("Select up to 16 assets to mix. Click a thumbnail to inspect it.");
    if (!state.active.empty()) {
        if (const auto* value = state.Source(state.active)) {
            ImGui::SeparatorText(value->name.c_str());
            if (void* texture = state.Preview(ctx, state.active + "#detail", *value, true)) {
                const float size = std::min(192.0f, ImGui::GetContentRegionAvail().x);
                ImGui::Image(widgets::ToImTextureID(texture), {size, size});
                if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    state.orbit.yaw += ImGui::GetIO().MouseDelta.x * 0.01f;
                    state.orbit.pitch = std::clamp(state.orbit.pitch + ImGui::GetIO().MouseDelta.y * 0.01f, -1.4f, 1.4f);
                }
            }
            if (ImGui::Button("Edit Paint Asset")) { LoadForm(state.form, state.active, *value); state.openForm = true; }
        }
    }
    if (state.openForm) { ImGui::OpenPopup("Paint Asset Settings"); state.openForm = false; }
    ImGui::SetNextWindowSize({520.0f, 650.0f}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Paint Asset Settings", nullptr, ImGuiWindowFlags_None)) {
        auto candidate = state.form.value;
        if (candidate.definition.Replace(state.form.parts, state.form.heights, {})) {
            if (void* texture = state.Preview(ctx, "#form", candidate, true)) ImGui::Image(widgets::ToImTextureID(texture), {128, 128});
        }
        const bool create = state.form.create;
        if (DrawForm(ctx, state.form, true) && create) {
            const auto path = ToProjectAssetDiskPath(ctx.projectRoot, state.form.path);
            state.active = path;
            const auto guid = asset::AssetDatabase::TryGetGuidFromPath(path);
            if (!guid.empty() && next.size() < 16) { next.push_back("guid:" + guid + "|" + NormalizeAssetPath(path)); changed = true; }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}

void TerrainPaintLibrary::DrawInspector(EditorContext& ctx, const std::string& path)
{
    m_impl->Begin(ctx);
    auto& form = m_impl->inspector;
    if (m_impl->inspectorPath != path) {
        const auto* source = Resolve(path);
        if (!source) { ImGui::TextWrapped("This Paint asset could not be loaded."); return; }
        LoadForm(form, path, *source); m_impl->inspectorPath = path;
    }
    ImGui::TextDisabled("Terrain Paint Asset");
    auto candidate = form.value;
    if (candidate.definition.Replace(form.parts, form.heights, {})) {
        if (void* texture = m_impl->Preview(ctx, "#inspector", candidate, true)) ImGui::Image(widgets::ToImTextureID(texture), {192, 192});
    }
    (void)DrawForm(ctx, form, false);
}

void DrawTerrainPaintAssetInspector(EditorContext& ctx, const std::string& path)
{
    if (ctx.terrainTool) { ctx.terrainTool->DrawPaintAssetInspector(ctx, path); return; }
    /// @note Inspector 側は GPU キャッシュを作らず、共通の編集フォームだけを使う。
    static PaintForm form;
    static std::string currentProject;
    if (currentProject != ctx.projectRoot || form.path != path) {
        const auto* source = Resolve(path);
        if (!source) { ImGui::TextWrapped("This Paint asset could not be loaded."); return; }
        LoadForm(form, path, *source); currentProject = ctx.projectRoot;
    }
    ImGui::TextDisabled("Terrain Paint Asset");
    (void)DrawForm(ctx, form, false);
}

void DrawMeshInstancesAssetInspector(EditorContext& ctx, const std::string& path)
{
    const auto* value = asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(path));
    if (!value || !value->instances.Snapshot()) { ImGui::TextWrapped("This mesh placement asset could not be loaded."); return; }
    ImGui::TextDisabled("Mesh Instances");
    ImGui::Text("Instances: %zu", value->instances.Snapshot()->placements.size());
    ImGui::Text("Mesh Parts: %zu", value->instances.Parts().size());
    ImGui::TextWrapped("Paint Source: %s", value->terrainPaintRef.c_str());
    ImGui::TextWrapped("Edit placements through Terrain Paint or the Instanced Mesh component.");
    (void)ctx;
}
} /// @note namespace fbzz::editor
