// FBZZ Engine
// InspectorPhysics.cpp | fbzz::editor
// Physics 系 Component の Inspector 描画
#include "InspectorPhysics.hpp"

namespace fbzz::editor {

namespace {

void DrawColliderCommon(scene::ColliderComponent& col)
{
    widgets::DragVec3("Center", col.center, 0.01f, -1000.0f, 1000.0f);
    ImGui::Checkbox("Is Trigger", &col.isTrigger);
    ImGui::DragFloat("Restitution", &col.material.restitution, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Static Friction", &col.material.staticFriction, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Dynamic Friction", &col.material.dynamicFriction, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Density", &col.material.density, 0.01f, 0.0f, 100000.0f);
}

void DrawAabbCollider(scene::AabbColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    widgets::DragVec3("Size", col.size, 0.01f, 0.001f, 1000.0f);
    // 形状パラメータの反映と姿勢同期は SyncColliderPreview (= ColliderSync) が行う。
    SyncColliderPreview(go, col);
}

void DrawBoxCollider(scene::BoxColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    widgets::DragVec3("Size", col.size, 0.01f, 0.001f, 1000.0f);
    SyncColliderPreview(go, col);
}

void DrawSphereCollider(scene::SphereColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    SyncColliderPreview(go, col);
}

void DrawCapsuleCollider(scene::CapsuleColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    ImGui::DragFloat("Half Height", &col.halfHeight, 0.01f, 0.001f, 1000.0f);
    SyncColliderPreview(go, col);
}

void DrawMeshCollider(scene::MeshColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col);
    widgets::AssetPathField("Mesh Path", col.meshPath, ".fbx,.fzmodel", projectRoot);
    ImGui::DragInt("Mesh Index", &col.meshIndex, 1.0f, 0, 1024);
    ImGui::Checkbox("Use Transform Scale", &col.useTransformScale);
    if (ImGui::Button("Rebuild From Renderer")) {
        col.collider.reset();
        auto mesh = SourceMeshFromGameObject(go, col.meshPath, col.meshIndex);
        BuildMeshCollider(col, mesh);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rebuild From FBX")) {
        col.collider.reset();
        BuildMeshCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
    }
    if (!col.collider)
        ImGui::TextDisabled("No mesh data. Drop FBX or rebuild from renderer.");
    SyncColliderPreview(go, col);
}

void DrawTerrainCollider(scene::TerrainColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    if (!go.GetComponent<scene::TerrainComponent>())
        ImGui::TextColored({ 1.0f, 0.6f, 0.2f, 1.0f }, "TerrainComponent が同じ GO に必要です");
    SyncColliderPreview(go, col);
}

void DrawConvexHullCollider(scene::ConvexHullColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col);
    widgets::AssetPathField("Mesh Path", col.meshPath, ".fbx,.fzmodel", projectRoot);
    ImGui::DragInt("Mesh Index", &col.meshIndex, 1.0f, 0, 1024);
    ImGui::Checkbox("Use Transform Scale", &col.useTransformScale);
    if (ImGui::Button("Rebuild From Renderer")) {
        col.collider.reset();
        auto mesh = SourceMeshFromGameObject(go, col.meshPath, col.meshIndex);
        BuildConvexHullCollider(col, mesh);
    }
    ImGui::SameLine();
    if (ImGui::Button("Rebuild From FBX")) {
        col.collider.reset();
        BuildConvexHullCollider(col, MeshFromModelPath(col.meshPath, col.meshIndex));
    }
    if (!col.collider)
        ImGui::TextDisabled("No hull data. Drop FBX or rebuild from renderer.");
    SyncColliderPreview(go, col);
}

// メッシュ系 Collider の Undo スナップショット型。
// WHY: runtime 所有物の unique_ptr<Collider> と ColliderHandle をコピー対象から外し、
//      Undo 時に physics body を不必要に無効化しない。
//      geometry (meshPath/meshIndex/useTransformScale) が変わった場合のみ collider をリセットする。
struct MeshColliderValue {
    physics::PhysicsMaterial material;
    math::Vector3 center;
    std::string meshPath;
    int meshIndex = 0;
    bool isTrigger = false;
    bool useTransformScale = true;
    bool enabled = true;
};

MeshColliderValue CaptureMeshColliderValue(const scene::MeshColliderComponent& c)
{
    return { c.material, c.center, c.meshPath, c.meshIndex, c.isTrigger, c.useTransformScale, c.enabled };
}

void ApplyMeshColliderValue(scene::MeshColliderComponent& c, const MeshColliderValue& v)
{
    const bool geomChanged = c.meshPath != v.meshPath
        || c.meshIndex != v.meshIndex
        || c.useTransformScale != v.useTransformScale;
    c.material = v.material;
    c.center = v.center;
    c.isTrigger = v.isTrigger;
    c.enabled = v.enabled;
    c.meshPath = v.meshPath;
    c.meshIndex = v.meshIndex;
    c.useTransformScale = v.useTransformScale;
    if (geomChanged) c.collider.reset();
}

MeshColliderValue CaptureConvexHullValue(const scene::ConvexHullColliderComponent& c)
{
    return { c.material, c.center, c.meshPath, c.meshIndex, c.isTrigger, c.useTransformScale, c.enabled };
}

void ApplyConvexHullValue(scene::ConvexHullColliderComponent& c, const MeshColliderValue& v)
{
    const bool geomChanged = c.meshPath != v.meshPath
        || c.meshIndex != v.meshIndex
        || c.useTransformScale != v.useTransformScale;
    c.material = v.material;
    c.center = v.center;
    c.isTrigger = v.isTrigger;
    c.enabled = v.enabled;
    c.meshPath = v.meshPath;
    c.meshIndex = v.meshIndex;
    c.useTransformScale = v.useTransformScale;
    if (geomChanged) c.collider.reset();
}

} // namespace

void DrawPhysicsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::AabbColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "AABB Collider",
        [go](scene::AabbColliderComponent& col, EditorContext&) {
            DrawAabbCollider(col, *go);
        });

    DrawComponentSection<scene::BoxColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Box Collider",
        [go](scene::BoxColliderComponent& col, EditorContext&) {
            DrawBoxCollider(col, *go);
        });

    DrawComponentSection<scene::SphereColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sphere Collider",
        [go](scene::SphereColliderComponent& col, EditorContext&) {
            DrawSphereCollider(col, *go);
        });

    DrawComponentSection<scene::CapsuleColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Capsule Collider",
        [go](scene::CapsuleColliderComponent& col, EditorContext&) {
            DrawCapsuleCollider(col, *go);
        });

    DrawComponentSectionCustom<scene::MeshColliderComponent, MeshColliderValue>(
        go, ctx, m_componentClipboard, m_componentClipboardType, "Mesh Collider",
        [go](scene::MeshColliderComponent& col, EditorContext& ctx2) {
            DrawMeshCollider(col, *go, ctx2.projectRoot);
        },
        [](const scene::MeshColliderComponent& c) { return CaptureMeshColliderValue(c); },
        [](scene::MeshColliderComponent& c, const MeshColliderValue& v) { ApplyMeshColliderValue(c, v); });

    DrawComponentSectionCustom<scene::ConvexHullColliderComponent, MeshColliderValue>(
        go, ctx, m_componentClipboard, m_componentClipboardType, "Convex Hull Collider",
        [go](scene::ConvexHullColliderComponent& col, EditorContext& ctx2) {
            DrawConvexHullCollider(col, *go, ctx2.projectRoot);
        },
        [](const scene::ConvexHullColliderComponent& c) { return CaptureConvexHullValue(c); },
        [](scene::ConvexHullColliderComponent& c, const MeshColliderValue& v) { ApplyConvexHullValue(c, v); });

    DrawComponentSection<scene::TerrainColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain Collider",
        [go](scene::TerrainColliderComponent& col, EditorContext&) {
            DrawTerrainCollider(col, *go);
        });

    DrawComponentSection<scene::RigidBodyComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Rigid Body",
        [](scene::RigidBodyComponent& rb, EditorContext&) {
            if (!rb.rigidBody) {
                ImGui::TextDisabled("No physics::RigidBody assigned");
                return;
            }

            auto& body = *rb.rigidBody;
            bool isStatic = body.IsStatic();
            if (ImGui::Checkbox("Static", &isStatic)) {
                body.m_isStatic = isStatic;
                body.SetMass(body.GetMass());
            }

            float mass = body.GetMass();
            if (ImGui::DragFloat("Mass", &mass, 0.05f, 0.0f, 100000.0f))
                body.SetMass(mass);

            math::Vector3 velocity = body.GetVelocity();
            if (widgets::DragVec3("Velocity", velocity, 0.05f))
                body.SetVelocity(velocity);

            math::Vector3 angularVelocity = body.GetAngularVelocity();
            if (widgets::DragVec3("Angular Velocity", angularVelocity, 0.05f))
                body.SetAngularVelocity(angularVelocity);

            auto freezePosition = body.GetFreezePosition();
            if (ImGui::Checkbox("Freeze Position X", &freezePosition.x))
                body.SetFreezePosition(freezePosition);
            ImGui::SameLine();
            if (ImGui::Checkbox("Y##FreezePosition", &freezePosition.y))
                body.SetFreezePosition(freezePosition);
            ImGui::SameLine();
            if (ImGui::Checkbox("Z##FreezePosition", &freezePosition.z))
                body.SetFreezePosition(freezePosition);

            auto freezeRotation = body.GetFreezeRotation();
            if (ImGui::Checkbox("Freeze Rotation X", &freezeRotation.x))
                body.SetFreezeRotation(freezeRotation);
            ImGui::SameLine();
            if (ImGui::Checkbox("Y##FreezeRotation", &freezeRotation.y))
                body.SetFreezeRotation(freezeRotation);
            ImGui::SameLine();
            if (ImGui::Checkbox("Z##FreezeRotation", &freezeRotation.z))
                body.SetFreezeRotation(freezeRotation);

            ImGui::Checkbox("Use Gravity", &body.m_useGravity);
            ImGui::DragFloat("Gravity Scale", &body.m_gravityScale, 0.01f, -100.0f, 100.0f);
            ImGui::DragFloat("Linear Drag", &body.m_linearDrag, 0.01f, 0.0f, 1000.0f);
            ImGui::DragFloat("Angular Drag", &body.m_angularDrag, 0.01f, 0.0f, 1000.0f);
            ImGui::Checkbox("Allow Sleeping", &body.m_allowSleeping);
            ImGui::Checkbox("Use CCD", &body.m_useCCD);
            ImGui::DragFloat("CCD Radius", &body.m_ccdRadius, 0.01f, 0.001f, 1000.0f);
            ImGui::DragFloat("Charge", &body.m_charge, 0.01f, -1000.0f, 1000.0f);
            ImGui::Checkbox("Gravity Source", &body.m_isGravitationalSource);
            ImGui::DragFloat("Gravity Mass", &body.m_gravitationalMass, 0.05f, 0.0f, 100000.0f);
        });

    DrawComponentSection<scene::VolumeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Volume",
        [](scene::VolumeComponent& volume, EditorContext&) {
            static constexpr const char* kVolumeNames[] = {
                "Gravity", "Vortex", "Buoyancy", "Explosion", "Time Dilation", "Magnetic"
            };
            int typeIdx = static_cast<int>(volume.type);
            if (ImGui::Combo("Type", &typeIdx, kVolumeNames, 6))
                volume.type = static_cast<physics::VolumeType>(typeIdx);

            widgets::DragVec3("Gravity", volume.gravity, 0.05f);
            widgets::DragVec3("Magnetic Field", volume.magneticField, 0.05f);
            ImGui::DragFloat("Swirl", &volume.swirlStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Inward", &volume.inwardStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Lift", &volume.liftStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Buoyancy", &volume.buoyancy, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Drag", &volume.drag, 0.01f, 0.0f, 100.0f);
            ImGui::DragFloat("Explosion Impulse", &volume.explosionImpulse, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Time Scale", &volume.timeScale, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Duration", &volume.duration, 0.05f, -1.0f, 1000.0f);
        });

    DrawComponentSection<scene::CharacterControllerComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Character Controller",
        [](scene::CharacterControllerComponent& cc, EditorContext&) {
            ImGui::DragFloat("Jump Min Air Time",        &cc.jumpMinAirTime,          0.01f, 0.0f, 2.0f);
            ImGui::DragFloat("Fall Vel Threshold",       &cc.fallVelThreshold,        0.1f, -50.0f, 0.0f);
            ImGui::DragFloat("Ground Vel Threshold",     &cc.groundVelThreshold,      0.01f, 0.0f, 5.0f);
            ImGui::DragFloat("Ledge Fall Threshold",     &cc.ledgeFallThreshold,      0.1f, -50.0f, 0.0f);
            ImGui::DragFloat("Min Ground Normal Y",      &cc.minGroundNormalY,        0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Ground Contact Grace",     &cc.groundContactGrace,      0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Jump Ground Ignore Time",  &cc.jumpGroundIgnoreTime,    0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Grounded Vel Snap",        &cc.groundedVelSnap,         0.01f, 0.0f, 5.0f);
            ImGui::DragFloat("Intentional Jump MaxTime", &cc.intentionalJumpMaxTime,  0.05f, 0.0f, 5.0f);
            ImGui::Spacing();
            ImGui::BeginDisabled();
            ImGui::Checkbox("Is Grounded (runtime)", &cc.isGrounded);
            ImGui::DragFloat("Vertical Speed (runtime)", &cc.verticalSpeed, 0.0f);
            ImGui::EndDisabled();
        });

}


} // namespace fbzz::editor
