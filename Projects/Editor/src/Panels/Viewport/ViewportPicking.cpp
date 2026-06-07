// FBZZ Engine
// ViewportPicking.cpp | fbzz::editor
// Scene View の Prefab ドロップと3Dピッキング
#include "ViewportCommon.hpp"

namespace fbzz::editor {

math::Matrix4 ToColumnMajor(const math::Matrix4& rowMajor)
{
    return math::Matrix4::Transpose(rowMajor);
}

math::Ray ScreenRayFromMouse(const EditorContext& ctx, const ImVec2& viewportMin)
{
    ImVec2 mouse = ImGui::GetMousePos();
    float nx = ((mouse.x - viewportMin.x) / ctx.viewportWidth)  * 2.0f - 1.0f;
    float ny = 1.0f - ((mouse.y - viewportMin.y) / ctx.viewportHeight) * 2.0f;

    const math::Matrix4 invVP = math::Matrix4::Inverse(
        ctx.editorCamera->GetProjectionMatrix() * ctx.editorCamera->GetViewMatrix());
    return math::Ray::FromNDC(nx, ny, ctx.editorCamera->m_position, invVP);
}

bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath)
{
    if (!payload || payload->DataSize <= 0) return false;
    outPath.assign(static_cast<const char*>(payload->Data),
                   static_cast<size_t>(payload->DataSize - 1));
    return !outPath.empty();
}

math::Vector3 PrefabDropPosition(const EditorContext& ctx, const ImVec2& viewportMin)
{
    const math::Ray ray = ScreenRayFromMouse(ctx, viewportMin);
    float planeT = 0.0f;
    if (ray.IntersectPlane(math::Plane({ 0.0f, 1.0f, 0.0f }, 0.0f), planeT) && planeT > 0.0f)
        return ray.At(planeT);
    return ray.At(5.0f);
}

bool InstantiatePrefabAsset(EditorContext& ctx, const std::string& assetPath)
{
    if (!ctx.activeScene || util::FileSystem::GetExtension(assetPath) != ".fbzzprefab")
        return false;

    std::vector<scene::EntityID> roots;
    if (!PrefabSerializer::Instantiate(*ctx.activeScene, assetPath, roots))
        return false;

    ctx.selectedEntities = roots;
    return true;
}

bool InstantiatePrefabAssetAtViewport(EditorContext& ctx,
                                      const std::string& assetPath,
                                      const ImVec2& viewportMin)
{
    if (!InstantiatePrefabAsset(ctx, assetPath))
        return false;

    const math::Vector3 position = PrefabDropPosition(ctx, viewportMin);
    for (scene::EntityID id : ctx.selectedEntities) {
        if (auto* go = ctx.activeScene->GetGameObject(id))
            go->transform.localPosition = position;
    }
    return true;
}

void HandleGizmoShortcuts(EditorContext& ctx)
{
    if (input::Input::KeyDown(input::KeyCode::W)) ctx.gizmoMode = EditorContext::GizmoMode::Translate;
    if (input::Input::KeyDown(input::KeyCode::E)) ctx.gizmoMode = EditorContext::GizmoMode::Rotate;
    if (input::Input::KeyDown(input::KeyCode::R)) ctx.gizmoMode = EditorContext::GizmoMode::Scale;
    if (input::Input::KeyDown(input::KeyCode::Q)) {
        ctx.gizmoSpace = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
            ? EditorContext::GizmoSpace::Local
            : EditorContext::GizmoSpace::World;
    }
}

bool PickEntity(EditorContext& ctx, const ImVec2& viewportMin)
{
    if (!ctx.activeScene || !ctx.editorCamera) return false;

    const math::Ray ray = ScreenRayFromMouse(ctx, viewportMin);

    scene::EntityID best         = scene::EntityID::INVALID;
    float           bestT        = 1e30f;
    scene::EntityID bestFallback = scene::EntityID::INVALID;
    float           bestFallbackT = 1e30f;

    auto transformPoint = [](const math::Matrix4& m, const math::Vector3& p) {
        math::Vector4 v = m * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (!math::NearlyZero(v.w)) v = v * (1.0f / v.w);
        return math::Vector3{ v.x, v.y, v.z };
    };

    auto testMesh = [&](const auto& verts,
                        const auto& indices,
                        const math::Matrix4& world,
                        scene::EntityID id) {
        if (verts.empty() || indices.size() < 3) return;

        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const uint32_t i0 = indices[i + 0];
            const uint32_t i1 = indices[i + 1];
            const uint32_t i2 = indices[i + 2];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;

            const math::Vector3 v0 = transformPoint(world, verts[i0].position);
            const math::Vector3 v1 = transformPoint(world, verts[i1].position);
            const math::Vector3 v2 = transformPoint(world, verts[i2].position);

            float t = 0.0f;
            if (ray.IntersectTriangle(v0, v1, v2, t) && t < bestT) {
                bestT = t;
                best  = id;
            }
        }
    };

    for (auto& go : ctx.activeScene->GameObjects()) {
        auto* mr = go.GetComponent<scene::MeshRenderer>();
        const math::Matrix4 world = go.transform.GetWorldMatrix();
        if (mr && mr->mesh) {
            testMesh(mr->mesh->cpuVertices, mr->mesh->cpuIndices, world, go.GetID());
        }

        if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>(); smr && smr->model) {
            for (const auto& meshPtr : smr->model->meshes) {
                if (!meshPtr) continue;
                testMesh(meshPtr->cpuSkinnedVertices, meshPtr->cpuIndices, world, go.GetID());
            }
        }

        const math::Vector3 center = go.transform.position;
        const float radius = (std::max)(0.5f, go.transform.worldScale.Length() / 3.0f);
        float t = 0.0f;
        if (ray.IntersectSphere(center, radius, t) && t < bestFallbackT) {
            bestFallbackT = t;
            bestFallback  = go.GetID();
        }
    }

    if (!best.IsValid() && bestFallback.IsValid()) {
        best = bestFallback;
        bestT = bestFallbackT;
    }

    if (best.IsValid() && !ctx.IsLocked(best)) {
        if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
        if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), best) == ctx.selectedEntities.end())
            ctx.selectedEntities.push_back(best);
        return true;
    }

    if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
    return false;
}

} // namespace fbzz::editor
