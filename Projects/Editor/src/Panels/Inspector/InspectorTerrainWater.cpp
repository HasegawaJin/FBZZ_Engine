// FBZZ Engine
// InspectorTerrainWater.cpp | fbzz::editor
// Terrain / Water 系 Component の Inspector 描画
#include "InspectorTerrainWater.hpp"

namespace fbzz::editor {

void DrawTerrainWaterInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::TerrainComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain",
        [go](scene::TerrainComponent& tc, EditorContext& ctx) {
            // ── 外部 Terrain Asset ─────────────────────────────────────────────
            ImGui::SeparatorText("Asset");
            {
                char pathBuf[512];
                std::snprintf(pathBuf, sizeof(pathBuf), "%s", tc.terrainAssetPath.c_str());
                if (ImGui::InputText("Asset Path", pathBuf, sizeof(pathBuf)))
                    tc.terrainAssetPath = NormalizeAssetPath(pathBuf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        std::string path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        if (util::FileSystem::GetExtension(path) == ".fbzzterrain")
                            tc.terrainAssetPath = path;
                    }
                    ImGui::EndDragDropTarget();
                }

                if (tc.terrainAssetPath.empty()) {
                    if (ImGui::Button("Create Terrain Asset")) {
                        const std::string path = UniqueTerrainAssetPath(ctx, go ? go->name : "Terrain");
                        if (scene::TerrainAssetSerializer::Save(tc, TerrainAssetDiskPath(ctx, path))) {
                            tc.terrainAssetPath = path;
                            ctx.requestAssetBrowserRefresh = true;
                        }
                    }
                } else {
                    if (ImGui::Button("Save Asset")) {
                        scene::TerrainAssetSerializer::Save(tc, TerrainAssetDiskPath(ctx, tc.terrainAssetPath));
                        ctx.requestAssetBrowserRefresh = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Load Asset")) {
                        const bool enabled = tc.enabled;
                        const std::string path = tc.terrainAssetPath;
                        if (scene::TerrainAssetSerializer::Load(TerrainAssetDiskPath(ctx, path), tc)) {
                            tc.enabled = enabled;
                            tc.terrainAssetPath = path;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Unlink")) {
                        tc.terrainAssetPath.clear();
                    }
                }
            }

            // ── グリッド設定 ──────────────────────────────────────────────────
            ImGui::SeparatorText("Grid");
            if (ImGui::DragInt("Columns",    &tc.columns,   1.0f, 2, 4097))
                tc.heightDirty = true;
            if (ImGui::DragInt("Rows",       &tc.rows,      1.0f, 2, 4097))
                tc.heightDirty = true;
            if (ImGui::DragFloat("Cell Size",   &tc.cellSize,  0.01f, 0.01f, 100.0f))
                tc.heightDirty = true;
            if (ImGui::DragFloat("Max Height",  &tc.maxHeight, 0.5f, 0.5f, 1000.0f))
                tc.heightDirty = true;
            ImGui::DragInt("Chunk Size", &tc.chunkSize, 1.0f, 8, 256);

            // ── Material (.fzmat) ─────────────────────────────────────────────
            ImGui::SeparatorText("Material (.fzmat)");
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", tc.materialPath.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f);
                if (ImGui::InputText("##terrain_mat_path", buf, sizeof(buf))) {
                    tc.materialPath = NormalizeAssetPath(buf);
                    tc.splatDirty = true;
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        if (dropped.size() > 6 && dropped.substr(dropped.size() - 6) == ".fzmat") {
                            tc.materialPath = dropped;
                            tc.splatDirty = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (tc.materialPath.empty()) {
                    ImGui::TextDisabled("(no material — layer textures missing)");
                    if (ImGui::Button("Use Default Terrain Material")) {
                        tc.materialPath = DefaultTerrainMaterialPath();
                        tc.splatDirty = true;
                    }
                }
            }

            // ── コライダー ─────────────────────────────────────────────────────
            ImGui::SeparatorText("Collider");
            if (ImGui::Button("Rebuild Collider Now")) {
                tc.colliderDirty = true;
            }

            // ── デバッグ情報 ───────────────────────────────────────────────────
            ImGui::SeparatorText("Info");
            ImGui::Text("Vertices : %d", tc.columns * tc.rows);
            ImGui::Text("Triangles: %d", (tc.columns - 1) * (tc.rows - 1) * 2);
            ImGui::TextColored(tc.heightDirty   ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Height Dirty : %s", tc.heightDirty   ? "Yes" : "No");
            ImGui::TextColored(tc.colliderDirty ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Collider Dirty: %s", tc.colliderDirty ? "Yes" : "No");
        });

    DrawComponentSection<scene::WaterComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Water",
        [go](scene::WaterComponent& water, EditorContext&) {
            // ── Material (.fzmat) ─────────────────────────────────────────────
            ImGui::SeparatorText("Material (.fzmat)");
            char matBuf[256];
            std::snprintf(matBuf, sizeof(matBuf), "%s", water.materialPath.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f);
            if (ImGui::InputText("##water_mat_path", matBuf, sizeof(matBuf))) {
                water.materialPath = NormalizeAssetPath(matBuf);
                water.texDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    if (dropped.size() > 6 && dropped.substr(dropped.size() - 6) == ".fzmat") {
                        water.materialPath = dropped;
                        water.texDirty = true;
                        water.foamDirty = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (water.materialPath.empty()) {
                ImGui::TextDisabled("(no material — visual params missing)");
                if (ImGui::Button("Use Default Water Material")) {
                    water.materialPath = DefaultWaterMaterialPath();
                    water.texDirty = true;
                    water.foamDirty = true;
                }
            }

            // ── Geometry ─────────────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::SeparatorText("Geometry");
            int resX = static_cast<int>(water.resolutionX);
            int resZ = static_cast<int>(water.resolutionZ);
            if (ImGui::DragInt("Resolution X", &resX, 1.0f, 1, 512)) {
                water.resolutionX = static_cast<uint32_t>(resX);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragInt("Resolution Z", &resZ, 1.0f, 1, 512)) {
                water.resolutionZ = static_cast<uint32_t>(resZ);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent X", &water.extentX, 0.5f, 0.1f, 10000.0f)) {
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent Z", &water.extentZ, 0.5f, 0.1f, 10000.0f)) {
                water.meshDirty = true;
                water.foamDirty = true;
            }
            {
                int chunks = static_cast<int>(water.chunkCount);
                if (ImGui::DragInt("Chunk Count", &chunks, 1.0f, 1, 64)) {
                    water.chunkCount = static_cast<uint32_t>(chunks < 1 ? 1 : chunks);
                    water.meshDirty = true;
                }
                ImGui::TextDisabled("(%d x %d chunks = %d draw calls)", chunks, chunks, chunks * chunks);
            }

            // ── Physics ───────────────────────────────────────────────────────
            ImGui::SeparatorText("Physics");
            if (ImGui::Button("Setup Buoyancy Volume")) {
                auto* box = go->GetComponent<scene::BoxColliderComponent>();
                if (!box) box = &go->AddComponent<scene::BoxColliderComponent>();
                box->enabled   = true;
                box->isTrigger = true;
                box->center    = { 0.0f, -2.5f, 0.0f };
                box->size      = { water.extentX, 5.0f, water.extentZ };

                auto* volume = go->GetComponent<scene::VolumeComponent>();
                if (!volume) volume = &go->AddComponent<scene::VolumeComponent>();
                volume->enabled  = true;
                volume->type     = physics::VolumeType::Buoyancy;
                volume->buoyancy = 15.0f;
                volume->drag     = 2.0f;
                volume->duration = -1.0f;
                volume->elapsed  = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Sync Collider Size")) {
                if (auto* box = go->GetComponent<scene::BoxColliderComponent>()) {
                    box->isTrigger = true;
                    box->size      = { water.extentX, box->size.y, water.extentZ };
                }
            }

            // ── Gerstner Waves ────────────────────────────────────────────────
            ImGui::SeparatorText("Gerstner Waves");
            ImGui::Checkbox("Enable Waves", &water.enableGerstnerWaves);
            for (int i = 0; i < static_cast<int>(water.waves.size()); ++i) {
                auto& wave = water.waves[static_cast<size_t>(i)];
                ImGui::PushID(i);
                if (ImGui::TreeNodeEx("Wave", ImGuiTreeNodeFlags_DefaultOpen, "Wave %d", i)) {
                    DragVec2("Direction", wave.direction, 0.01f, -1.0f, 1.0f);
                    ImGui::DragFloat("Amplitude",  &wave.amplitude,  0.01f, 0.0f,  100.0f);
                    ImGui::DragFloat("Wavelength", &wave.wavelength, 0.1f,  0.01f, 10000.0f);
                    ImGui::DragFloat("Steepness",  &wave.steepness,  0.01f, 0.0f,  1.0f);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        });

}


} // namespace fbzz::editor
