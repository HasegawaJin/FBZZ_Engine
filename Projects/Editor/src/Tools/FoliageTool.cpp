// FBZZ Engine
// FoliageTool.cpp | fbzz::editor
// Foliage スタンプツールの入力・Terrain レイキャスト・Undo/Redo 実装
#include "FoliageTool.hpp"
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

namespace fbzz::editor {

namespace {

constexpr float kPi = 3.14159265f;

float Random01(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
}

math::Vector3 TransformPoint(const math::Matrix4& matrix, const math::Vector3& point)
{
    const math::Vector4 transformed =
        matrix * math::Vector4{ point.x, point.y, point.z, 1.0f };
    return { transformed.x, transformed.y, transformed.z };
}

} // namespace

void FoliageTool::Update(scene::Scene& scene,
                         const renderer::Camera& camera,
                         bool viewportHovered,
                         const ImVec2& viewportMin,
                         const ImVec2& viewportSize,
                         const std::function<void()>& markDirty,
                         UndoStack* undoStack)
{
    if (!m_active || !viewportHovered) {
        m_isHovering = false;
        return;
    }

    // 消去半径のホットキー調整（TerrainTool / DetailTool と統一）。[ / ] で Erase Radius を増減。
    {
        const float step = std::max(0.5f, m_eraseRadius * 0.1f);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, /*repeat=*/true))
            m_eraseRadius = std::clamp(m_eraseRadius - step, 0.5f, 30.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, /*repeat=*/true))
            m_eraseRadius = std::clamp(m_eraseRadius + step, 0.5f, 30.0f);
    }

    m_isHovering = RaycastTerrain(
        scene, camera, viewportMin, viewportSize,
        m_hitPoint, m_hitLocal, m_hitEntity);
    const bool erase = ImGui::GetIO().KeyShift;
    if (m_isHovering)
        DrawStampPreview(viewportMin, viewportSize, camera, erase);

    if (!m_isHovering
        || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        || ImGui::GetIO().KeyAlt) {
        return;
    }

    auto* foliage = scene.GetComponent<scene::FoliageComponent>(m_hitEntity);
    auto* gameObject = scene.GetGameObject(m_hitEntity);
    if (!foliage || !gameObject || m_hitEntity != m_targetEntity
        || m_speciesIndex < 0
        || m_speciesIndex >= static_cast<int>(foliage->species.size())) {
        return;
    }

    const size_t speciesIndex = static_cast<size_t>(m_speciesIndex);
    const scene::FoliageSpecies before = foliage->species[speciesIndex];
    scene::FoliageSpecies after = before;
    after.placementMode = scene::FoliagePlacementMode::STAMP;

    if (erase) {
        const math::Matrix4 world = gameObject->transform.GetWorldMatrix();
        std::erase_if(after.stamps, [&](const scene::FoliageStamp& stamp) {
            const math::Vector3 stampWorld = TransformPoint(world, stamp.localPosition);
            const float dx = stampWorld.x - m_hitPoint.x;
            const float dz = stampWorld.z - m_hitPoint.z;
            return dx * dx + dz * dz <= m_eraseRadius * m_eraseRadius;
        });
        if (after.stamps.size() == before.stamps.size())
            return;
    } else {
        const float minScale = std::max(
            0.01f, std::min(after.minScale, after.maxScale));
        const float maxScale = std::max(
            minScale, std::max(after.minScale, after.maxScale));
        scene::FoliageStamp stamp{};
        stamp.localPosition = m_hitLocal;
        stamp.rotationY = after.randomYRotation
            ? Random01(m_randomState) * 2.0f * kPi : 0.0f;
        stamp.scale = minScale + Random01(m_randomState) * (maxScale - minScale);
        after.stamps.push_back(stamp);
    }

    const scene::EntityID entity = m_hitEntity;
    auto apply = [this, &scene, entity, speciesIndex, markDirty](
                     const scene::FoliageSpecies& state) {
        ApplySpeciesState(scene, entity, speciesIndex, state, markDirty);
    };

    if (undoStack) {
        undoStack->Execute(std::make_unique<LambdaCommand>(
            erase ? "Erase Foliage Stamps" : "Place Foliage Stamp",
            [apply, after]() { apply(after); },
            [apply, before]() { apply(before); }));
    } else {
        apply(after);
    }
}

void FoliageTool::ApplySpeciesState(
    scene::Scene& scene,
    scene::EntityID entity,
    size_t speciesIndex,
    const scene::FoliageSpecies& state,
    const std::function<void()>& markDirty) const
{
    auto* foliage = scene.GetComponent<scene::FoliageComponent>(entity);
    if (!foliage || speciesIndex >= foliage->species.size())
        return;
    foliage->species[speciesIndex] = state;
    foliage->needsBake         = true;
    foliage->needsBakeChildren = true;
    if (markDirty)
        markDirty();
}

bool FoliageTool::RaycastTerrain(
    scene::Scene& scene,
    const renderer::Camera& camera,
    const ImVec2& viewportMin,
    const ImVec2& viewportSize,
    math::Vector3& outHitWorld,
    math::Vector3& outHitLocal,
    scene::EntityID& outEntity) const
{
    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
        return false;

    const ImVec2 mouse = ImGui::GetMousePos();
    const float ndcX = ((mouse.x - viewportMin.x) / viewportSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - viewportMin.y) / viewportSize.y) * 2.0f;
    const math::Matrix4 inverseViewProjection = math::Matrix4::Inverse(
        camera.GetProjectionMatrix() * camera.GetViewMatrix());
    const math::Ray ray =
        math::Ray::FromNDC(ndcX, ndcY, camera.m_position, inverseViewProjection);

    float bestDistance = 1e30f;
    scene::EntityID bestEntity{};
    math::Vector3 bestWorld{};
    math::Vector3 bestLocal{};
    for (scene::EntityID entity : scene.GetEntities<scene::TerrainComponent>()) {
        auto* terrain = scene.GetComponent<scene::TerrainComponent>(entity);
        auto* gameObject = scene.GetGameObject(entity);
        if (!terrain || !gameObject || !terrain->enabled || terrain->heightData.empty())
            continue;

        math::Vector3 localHit{};
        if (!RaycastSingleTerrain(ray, *terrain, gameObject->transform, localHit))
            continue;
        const math::Vector3 worldHit =
            TransformPoint(gameObject->transform.GetWorldMatrix(), localHit);
        const float distance = math::Vector3::Dot(worldHit - ray.origin, ray.direction);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestEntity = entity;
            bestWorld = worldHit;
            bestLocal = localHit;
        }
    }

    if (!scene.IsValid(bestEntity))
        return false;
    outHitWorld = bestWorld;
    outHitLocal = bestLocal;
    outEntity = bestEntity;
    return true;
}

bool FoliageTool::RaycastSingleTerrain(
    const math::Ray& ray,
    const scene::TerrainComponent& terrain,
    const scene::Transform& transform,
    math::Vector3& outLocalHit) const
{
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.cellSize <= 0.0f)
        return false;

    const math::Matrix4 inverseWorld =
        math::Matrix4::Inverse(transform.GetWorldMatrix());
    const math::Vector4 localOrigin4 =
        inverseWorld * math::Vector4{ ray.origin.x, ray.origin.y, ray.origin.z, 1.0f };
    const math::Vector4 localDirection4 =
        inverseWorld * math::Vector4{ ray.direction.x, ray.direction.y, ray.direction.z, 0.0f };
    const math::Vector3 origin = {
        localOrigin4.x, localOrigin4.y, localOrigin4.z
    };
    const math::Vector3 direction = math::Vector3{
        localDirection4.x, localDirection4.y, localDirection4.z
    }.Normalized();

    const float terrainWidth =
        static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainDepth =
        static_cast<float>(terrain.rows - 1) * terrain.cellSize;
    auto intersectSlab = [](float rayOrigin, float rayDirection,
                            float minimum, float maximum,
                            float& nearDistance, float& farDistance) {
        if (std::abs(rayDirection) < 1e-6f)
            return rayOrigin >= minimum && rayOrigin <= maximum;
        float first = (minimum - rayOrigin) / rayDirection;
        float second = (maximum - rayOrigin) / rayDirection;
        if (first > second) std::swap(first, second);
        nearDistance = std::max(nearDistance, first);
        farDistance = std::min(farDistance, second);
        return nearDistance <= farDistance;
    };

    float nearDistance = 0.0f;
    float farDistance = 1e30f;
    if (!intersectSlab(origin.x, direction.x, 0.0f, terrainWidth,
                       nearDistance, farDistance)
        || !intersectSlab(origin.y, direction.y, -terrain.maxHeight, terrain.maxHeight,
                          nearDistance, farDistance)
        || !intersectSlab(origin.z, direction.z, 0.0f, terrainDepth,
                          nearDistance, farDistance)
        || farDistance <= 0.0f) {
        return false;
    }

    nearDistance = std::max(nearDistance, 0.0f);
    const float horizontalDirection =
        std::max(std::abs(direction.x), std::abs(direction.z));
    const float stepDistance = horizontalDirection > 1e-6f
        ? terrain.cellSize / horizontalDirection
        : terrain.cellSize;
    const int maxSteps =
        static_cast<int>((farDistance - nearDistance) / stepDistance) + 2;

    float previousDistance = nearDistance;
    float previousDelta = 0.0f;
    bool hasPrevious = false;
    float bracketNear = nearDistance;
    float bracketFar = nearDistance;
    for (int step = 0; step <= maxSteps; ++step) {
        const float distance = std::min(
            nearDistance + static_cast<float>(step) * stepDistance, farDistance);
        const math::Vector3 point = origin + direction * distance;
        const float delta = point.y - terrain.GetHeightAt(point.x, point.z);
        if (hasPrevious && previousDelta > 0.0f && delta <= 0.0f) {
            bracketNear = previousDistance;
            bracketFar = distance;
            break;
        }
        hasPrevious = true;
        previousDistance = distance;
        previousDelta = delta;
        if (distance >= farDistance)
            return false;
    }

    for (int iteration = 0; iteration < 8; ++iteration) {
        const float middle = (bracketNear + bracketFar) * 0.5f;
        const math::Vector3 point = origin + direction * middle;
        if (point.y > terrain.GetHeightAt(point.x, point.z))
            bracketNear = middle;
        else
            bracketFar = middle;
    }

    const float finalDistance = (bracketNear + bracketFar) * 0.5f;
    outLocalHit = origin + direction * finalDistance;
    outLocalHit.y = terrain.GetHeightAt(outLocalHit.x, outLocalHit.z);
    return true;
}

void FoliageTool::DrawStampPreview(
    const ImVec2& viewportMin,
    const ImVec2& viewportSize,
    const renderer::Camera& camera,
    bool erase) const
{
    const math::Matrix4 viewProjection = camera.GetViewProjection();
    auto project = [&](const math::Vector3& point) {
        const math::Vector4 clip =
            viewProjection * math::Vector4{ point.x, point.y, point.z, 1.0f };
        if (clip.w < 0.001f)
            return ImVec2{ -99999.0f, -99999.0f };
        return ImVec2{
            viewportMin.x + (clip.x / clip.w + 1.0f) * 0.5f * viewportSize.x,
            viewportMin.y + (1.0f - (clip.y / clip.w + 1.0f) * 0.5f) * viewportSize.y
        };
    };

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImU32 color = erase
        ? IM_COL32(230, 70, 60, 230)
        : IM_COL32(70, 220, 100, 230);
    const float radius = erase ? m_eraseRadius : 0.75f;
    ImVec2 previous = project({
        m_hitPoint.x + radius, m_hitPoint.y, m_hitPoint.z
    });
    for (int segment = 1; segment <= 32; ++segment) {
        const float angle =
            static_cast<float>(segment) / 32.0f * 2.0f * kPi;
        const ImVec2 current = project({
            m_hitPoint.x + std::cos(angle) * radius,
            m_hitPoint.y,
            m_hitPoint.z + std::sin(angle) * radius
        });
        drawList->AddLine(previous, current, color, 2.0f);
        previous = current;
    }
}

void FoliageTool::OnEditorGUI(
    scene::Scene& scene,
    const std::function<void()>& markDirty)
{
    const char* title =
        m_active ? "Foliage Tool###FoliageTool" : "Foliage Tool [OFF]###FoliageTool";
    if (!ImGui::Begin(title, nullptr,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor(
        ImGuiCol_Button,
        m_active
            ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f)
            : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    if (ImGui::Button(m_active ? "  Active  " : " Inactive ", { -1.0f, 0.0f }))
        m_active = !m_active;
    ImGui::PopStyleColor();

    if (!m_active)
        ImGui::BeginDisabled();
    DrawContent(scene, markDirty);
    if (!m_active)
        ImGui::EndDisabled();
    ImGui::End();
}

void FoliageTool::DrawContent(scene::Scene& scene,
                              const std::function<void()>& markDirty)
{
    ImGui::TextDisabled("Click: place  |  Shift+Click: erase  |  Alt: suspend");
    ImGui::SliderFloat("Erase Radius", &m_eraseRadius, 0.5f, 30.0f, "%.1f m");
    ImGui::TextDisabled("[ / ] : Erase Radius");
    ImGui::SeparatorText("Species");

    const auto* selectedFoliage = scene.IsValid(m_targetEntity)
        ? scene.GetComponent<scene::FoliageComponent>(m_targetEntity)
        : nullptr;
    if (!selectedFoliage
        || m_speciesIndex < 0
        || m_speciesIndex >= static_cast<int>(selectedFoliage->species.size())) {
        m_targetEntity = {};
        m_speciesIndex = 0;
    }

    bool foundSpecies = false;
    for (scene::EntityID entity : scene.GetEntities<scene::FoliageComponent>()) {
        auto* foliage = scene.GetComponent<scene::FoliageComponent>(entity);
        auto* gameObject = scene.GetGameObject(entity);
        if (!foliage || !gameObject || !foliage->enabled)
            continue;
        for (size_t index = 0; index < foliage->species.size(); ++index) {
            foundSpecies = true;
            const auto& species = foliage->species[index];
            if (!scene.IsValid(m_targetEntity)) {
                m_targetEntity = entity;
                m_speciesIndex = static_cast<int>(index);
            }
            const bool selected =
                m_targetEntity == entity && m_speciesIndex == static_cast<int>(index);
            const std::string label = "[" + std::to_string(index) + "] "
                + gameObject->name + " / "
                + (species.modelPath.empty() ? "(No Model)" : species.modelPath);
            ImGui::PushID(static_cast<int>(entity.index));
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::RadioButton(label.c_str(), selected)) {
                m_targetEntity = entity;
                m_speciesIndex = static_cast<int>(index);
            }
            if (selected) {
                ImGui::Indent();
                ImGui::TextDisabled("Stamps: %zu  |  Child GOs: %zu",
                    species.stamps.size(), foliage->childEntities.size());
                if (species.placementMode != scene::FoliagePlacementMode::STAMP)
                    ImGui::TextColored(
                        { 1.0f, 0.75f, 0.25f, 1.0f },
                        "First click switches this Species to Stamp mode.");

                auto* editSpecies = &foliage->species[index];
                auto markSpeciesEdited = [&]() {
                    foliage->needsBake = true;
                    foliage->needsBakeChildren = true;
                    if (markDirty)
                        markDirty();
                };

                // スタンプ配置設定。
                // WHY: Inspector を開かずに、FoliageTool だけで次に置く個体の見た目を調整できるようにする。
                ImGui::Spacing();
                ImGui::SeparatorText("Stamp Placement");
                float scaleRange[2] = { editSpecies->minScale, editSpecies->maxScale };
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::DragFloat2("Scale Range", scaleRange, 0.01f, 0.01f, 20.0f, "%.2f")) {
                    editSpecies->minScale = std::max(0.01f, scaleRange[0]);
                    editSpecies->maxScale = std::max(0.01f, scaleRange[1]);
                    if (editSpecies->minScale > editSpecies->maxScale)
                        std::swap(editSpecies->minScale, editSpecies->maxScale);
                    markSpeciesEdited();
                }
                if (ImGui::Checkbox("Random Y Rotation##stampRot", &editSpecies->randomYRotation))
                    markSpeciesEdited();

                // コライダー設定 (STAMP モード専用)
                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::SeparatorText("Collider (Box / OBB)");
                if (ImGui::Checkbox("Enabled##col", &editSpecies->colliderEnabled))
                    markSpeciesEdited();
                if (editSpecies->colliderEnabled) {
                    if (ImGui::Checkbox("Manual Override##colManual", &editSpecies->colliderManual))
                        markSpeciesEdited();
                    if (editSpecies->colliderManual) {
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::DragFloat("Half Width##colW", &editSpecies->colliderHalfWidth,
                                             0.01f, 0.01f, 10.0f, "%.2f"))
                            markSpeciesEdited();
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::DragFloat("Half Height##colH", &editSpecies->colliderHalfHeight,
                                             0.05f, 0.01f, 50.0f, "%.2f"))
                            markSpeciesEdited();
                    } else {
                        ImGui::TextDisabled("Box size auto-calculated from model AABB");
                    }
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::DragFloat("Cull Distance##cullDist",
                                         &editSpecies->colliderCullDistance,
                                         1.0f, 0.0f, 500.0f, "%.0f m"))
                        markSpeciesEdited();
                    if (editSpecies->colliderCullDistance <= 0.0f)
                        ImGui::TextDisabled("0 = no distance culling");
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
            ImGui::PopID();
        }
    }

    if (!foundSpecies) {
        ImGui::TextDisabled("No Foliage species in scene.");
        ImGui::TextDisabled("Add FoliageComponent and a Species first.");
    }
    if (m_isHovering && m_hitEntity != m_targetEntity)
        ImGui::TextColored(
            { 1.0f, 0.45f, 0.25f, 1.0f },
            "Cursor Terrain differs from selected Species.");

}

} // namespace fbzz::editor
