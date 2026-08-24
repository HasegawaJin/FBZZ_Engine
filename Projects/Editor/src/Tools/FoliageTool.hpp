// FBZZ Engine
// FoliageTool.hpp | fbzz::editor
// Terrain 上へ大型植生を1個ずつ配置・削除するスタンプ編集ツール
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/Components/FoliageComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Math/Ray.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <imgui.h>

namespace fbzz::editor {

class UndoStack;

// FoliageTool — Species を選び、Viewport の Terrain 面へ離散的な植生を配置する。
// WHY: 樹木は連続ブラシより個体単位の位置調整が重要なため、クリック式スタンプを採用する。
class FoliageTool {
public:
    void Update(scene::Scene& scene,
                const renderer::Camera& camera,
                bool viewportHovered,
                const ImVec2& viewportMin,
                const ImVec2& viewportSize,
                const std::function<void()>& markDirty,
                UndoStack* undoStack);

    void OnEditorGUI(scene::Scene& scene,
                     const std::function<void()>& markDirty);
    void DrawContent(scene::Scene& scene,
                     const std::function<void()>& markDirty = {});

    void SetActive(bool active) { m_active = active; }
    [[nodiscard]] bool IsActive() const { return m_active; }

    void SetEraseRadius(float radius) { m_eraseRadius = radius; }
    [[nodiscard]] float GetEraseRadius() const { return m_eraseRadius; }

private:
    bool RaycastTerrain(scene::Scene& scene,
                        const renderer::Camera& camera,
                        const ImVec2& viewportMin,
                        const ImVec2& viewportSize,
                        math::Vector3& outHitWorld,
                        math::Vector3& outHitLocal,
                        scene::EntityID& outEntity) const;
    bool RaycastSingleTerrain(const math::Ray& ray,
                              const scene::TerrainComponent& terrain,
                              const scene::Transform& transform,
                              math::Vector3& outLocalHit) const;
    void DrawStampPreview(const ImVec2& viewportMin,
                          const ImVec2& viewportSize,
                          const renderer::Camera& camera,
                          bool erase) const;
    void ApplySpeciesState(scene::Scene& scene,
                           scene::EntityID entity,
                           size_t speciesIndex,
                           const scene::FoliageSpecies& state,
                           const std::function<void()>& markDirty) const;

    bool m_active = false;
    scene::EntityID m_targetEntity = {};
    int m_speciesIndex = 0;
    float m_eraseRadius = 4.0f;
    uint32_t m_randomState = 1;
    bool m_isHovering = false;
    math::Vector3 m_hitPoint = {};
    math::Vector3 m_hitLocal = {};
    scene::EntityID m_hitEntity = {};
};

} // namespace fbzz::editor
