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
    auto* box = col.collider && col.collider->GetType() == physics::ColliderType::AABB
        ? static_cast<physics::AABBCollider*>(col.collider.get())
        : nullptr;
    if (!box) {
        col.collider = std::make_unique<physics::AABBCollider>(col.size * 0.5f);
        box = static_cast<physics::AABBCollider*>(col.collider.get());
    }
    box->m_halfExtents = col.size * 0.5f;
    SyncColliderPreview(go, col);
}

void DrawBoxCollider(scene::BoxColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    widgets::DragVec3("Size", col.size, 0.01f, 0.001f, 1000.0f);
    auto* box = col.collider && col.collider->GetType() == physics::ColliderType::OBB
        ? static_cast<physics::OBBCollider*>(col.collider.get())
        : nullptr;
    if (!box) {
        col.collider = std::make_unique<physics::OBBCollider>(col.size * 0.5f);
        box = static_cast<physics::OBBCollider*>(col.collider.get());
    }
    box->m_halfExtents = col.size * 0.5f;
    SyncColliderPreview(go, col);
}

void DrawSphereCollider(scene::SphereColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    auto* sphere = col.collider && col.collider->GetType() == physics::ColliderType::SPHERE
        ? static_cast<physics::SphereCollider*>(col.collider.get())
        : nullptr;
    if (!sphere) {
        col.collider = std::make_unique<physics::SphereCollider>(col.radius);
        sphere = static_cast<physics::SphereCollider*>(col.collider.get());
    }
    sphere->m_radius = col.radius;
    SyncColliderPreview(go, col);
}

void DrawCapsuleCollider(scene::CapsuleColliderComponent& col, scene::GameObject& go)
{
    DrawColliderCommon(col);
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    ImGui::DragFloat("Half Height", &col.halfHeight, 0.01f, 0.001f, 1000.0f);
    auto* capsule = col.collider && col.collider->GetType() == physics::ColliderType::CAPSULE
        ? static_cast<physics::CapsuleCollider*>(col.collider.get())
        : nullptr;
    if (!capsule) {
        col.collider = std::make_unique<physics::CapsuleCollider>(col.radius, col.halfHeight);
        capsule = static_cast<physics::CapsuleCollider*>(col.collider.get());
    }
    capsule->m_radius = col.radius;
    capsule->m_halfHeight = col.halfHeight;
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

// メッシュ系 Collider の永続化対象だけを保持する。
// WHY: runtime 所有物の unique_ptr<Collider> をコピー対象から外し、Undo と Inspector UI を
//      コンポーネントの複雑なコピーコンストラクタやテンプレート展開に依存させない。
struct MeshColliderValue {
    physics::PhysicsMaterial material;
    math::Vector3 center;
    std::string meshPath;
    int meshIndex = 0;
    bool isTrigger = false;
    bool useTransformScale = true;
    bool enabled = true;
};

struct MeshColliderSectionOps {
    const std::type_info& componentType;
    std::size_t editSlot;
    scene::ColliderComponent* (*Get)(scene::GameObject&);
    scene::ColliderComponent& (*Add)(scene::GameObject&);
    void (*Remove)(scene::GameObject&);
    MeshColliderValue (*Capture)(const scene::ColliderComponent&);
    void (*Apply)(scene::ColliderComponent&, const MeshColliderValue&);
    void (*Draw)(scene::ColliderComponent&, scene::GameObject&, const std::string& projectRoot);
};

struct MeshColliderActiveEdit {
    scene::EntityID entityId;
    ImGuiID activeId = 0;
    MeshColliderValue before;
    bool active = false;
};

bool IsSameMeshColliderValue(const MeshColliderValue& lhs, const MeshColliderValue& rhs)
{
    return lhs.material.restitution == rhs.material.restitution
        && lhs.material.staticFriction == rhs.material.staticFriction
        && lhs.material.dynamicFriction == rhs.material.dynamicFriction
        && lhs.material.density == rhs.material.density
        && lhs.center.x == rhs.center.x
        && lhs.center.y == rhs.center.y
        && lhs.center.z == rhs.center.z
        && lhs.meshPath == rhs.meshPath
        && lhs.meshIndex == rhs.meshIndex
        && lhs.isTrigger == rhs.isTrigger
        && lhs.useTransformScale == rhs.useTransformScale
        && lhs.enabled == rhs.enabled;
}

void ApplyCommonMeshColliderValue(
    scene::ColliderComponent& component,
    const MeshColliderValue& value)
{
    component.material = value.material;
    component.center = value.center;
    component.isTrigger = value.isTrigger;
    component.enabled = value.enabled;
}

scene::ColliderComponent* GetMeshCollider(scene::GameObject& go)
{
    return go.GetComponent<scene::MeshColliderComponent>();
}

scene::ColliderComponent& AddMeshCollider(scene::GameObject& go)
{
    return go.AddComponent<scene::MeshColliderComponent>();
}

void RemoveMeshCollider(scene::GameObject& go)
{
    go.RemoveComponent<scene::MeshColliderComponent>();
}

MeshColliderValue CaptureMeshCollider(const scene::ColliderComponent& component)
{
    const auto& collider = static_cast<const scene::MeshColliderComponent&>(component);
    return {
        collider.material,
        collider.center,
        collider.meshPath,
        collider.meshIndex,
        collider.isTrigger,
        collider.useTransformScale,
        collider.enabled
    };
}

void ApplyMeshCollider(scene::ColliderComponent& component, const MeshColliderValue& value)
{
    auto& collider = static_cast<scene::MeshColliderComponent&>(component);
    const bool geometryChanged = collider.meshPath != value.meshPath
        || collider.meshIndex != value.meshIndex
        || collider.useTransformScale != value.useTransformScale;
    ApplyCommonMeshColliderValue(collider, value);
    collider.meshPath = value.meshPath;
    collider.meshIndex = value.meshIndex;
    collider.useTransformScale = value.useTransformScale;
    if (geometryChanged) collider.collider.reset();
}

void DrawMeshColliderSectionBody(scene::ColliderComponent& component, scene::GameObject& go, const std::string& projectRoot)
{
    DrawMeshCollider(static_cast<scene::MeshColliderComponent&>(component), go, projectRoot);
}

scene::ColliderComponent* GetConvexHullCollider(scene::GameObject& go)
{
    return go.GetComponent<scene::ConvexHullColliderComponent>();
}

scene::ColliderComponent& AddConvexHullCollider(scene::GameObject& go)
{
    return go.AddComponent<scene::ConvexHullColliderComponent>();
}

void RemoveConvexHullCollider(scene::GameObject& go)
{
    go.RemoveComponent<scene::ConvexHullColliderComponent>();
}

MeshColliderValue CaptureConvexHullCollider(const scene::ColliderComponent& component)
{
    const auto& collider = static_cast<const scene::ConvexHullColliderComponent&>(component);
    return {
        collider.material,
        collider.center,
        collider.meshPath,
        collider.meshIndex,
        collider.isTrigger,
        collider.useTransformScale,
        collider.enabled
    };
}

void ApplyConvexHullCollider(
    scene::ColliderComponent& component,
    const MeshColliderValue& value)
{
    auto& collider = static_cast<scene::ConvexHullColliderComponent&>(component);
    const bool geometryChanged = collider.meshPath != value.meshPath
        || collider.meshIndex != value.meshIndex
        || collider.useTransformScale != value.useTransformScale;
    ApplyCommonMeshColliderValue(collider, value);
    collider.meshPath = value.meshPath;
    collider.meshIndex = value.meshIndex;
    collider.useTransformScale = value.useTransformScale;
    if (geometryChanged) collider.collider.reset();
}

void DrawConvexHullColliderSectionBody(
    scene::ColliderComponent& component,
    scene::GameObject& go,
    const std::string& projectRoot)
{
    DrawConvexHullCollider(
        static_cast<scene::ConvexHullColliderComponent&>(component),
        go,
        projectRoot);
}

const MeshColliderSectionOps MESH_COLLIDER_OPS {
    typeid(scene::MeshColliderComponent),
    0,
    &GetMeshCollider,
    &AddMeshCollider,
    &RemoveMeshCollider,
    &CaptureMeshCollider,
    &ApplyMeshCollider,
    &DrawMeshColliderSectionBody
};

const MeshColliderSectionOps CONVEX_HULL_COLLIDER_OPS {
    typeid(scene::ConvexHullColliderComponent),
    1,
    &GetConvexHullCollider,
    &AddConvexHullCollider,
    &RemoveConvexHullCollider,
    &CaptureConvexHullCollider,
    &ApplyConvexHullCollider,
    &DrawConvexHullColliderSectionBody
};

void PushMeshColliderValueCommand(
    scene::GameObject& go,
    EditorContext& ctx,
    const std::string& description,
    const MeshColliderValue& before,
    const MeshColliderValue& after,
    const MeshColliderSectionOps& ops)
{
    if (!CanRecordEditorUndo(ctx) || !ctx.activeScene
        || IsSameMeshColliderValue(before, after)) {
        return;
    }

    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;
    const MeshColliderSectionOps* operation = &ops;
    auto apply = [scene, instanceId, markDirty, operation](
        const MeshColliderValue& value) {
        if (auto* target = scene->FindByGuid(instanceId)) {
            if (auto* collider = operation->Get(*target)) {
                operation->Apply(*collider, value);
                if (markDirty) markDirty();
            }
        }
    };
    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        description,
        [apply, after]() { apply(after); },
        [apply, before]() { apply(before); }));
}

// メッシュ系 Collider 専用の非テンプレート Component Section を描画する。
// WHAT: Enable・Reset・Copy/Paste・Remove・連続編集 Undo を値スナップショットで統一する。
void DrawMeshColliderComponentSection(
    scene::GameObject* go,
    EditorContext& ctx,
    std::any& componentClipboard,
    const std::type_info*& componentClipboardType,
    const char* label,
    const MeshColliderSectionOps& ops)
{
    scene::ColliderComponent* component = ops.Get(*go);
    if (!component) return;

    ImGui::PushID(label);

    const bool canRecordUndo = CanRecordEditorUndo(ctx);
    MeshColliderValue before;
    if (canRecordUndo)
        before = ops.Capture(*component);
    if (ImGui::Checkbox("##en", &component->enabled)) {
        PushMeshColliderValueCommand(
            *go, ctx, std::string("Toggle ") + label,
            before, ops.Capture(*component), ops);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }
    ImGui::SameLine();

    const bool open = ImGui::CollapsingHeader(
        label,
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    const float buttonWidth = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonWidth);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("##comp_opts");

    bool removeRequested = false;
    if (ImGui::BeginPopup("##comp_opts")) {
        if (ImGui::MenuItem("Reset")) {
            before = ops.Capture(*component);
            MeshColliderValue resetValue;
            resetValue.material = physics::PhysicsMaterial::Default;
            resetValue.enabled = component->enabled;
            ops.Apply(*component, resetValue);
            PushMeshColliderValueCommand(
                *go, ctx, std::string("Reset ") + label,
                before, resetValue, ops);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Component")) {
            componentClipboard = ops.Capture(*component);
            componentClipboardType = &ops.componentType;
        }
        const bool canPaste = componentClipboardType
            && *componentClipboardType == ops.componentType
            && componentClipboard.type() == typeid(MeshColliderValue);
        if (ImGui::MenuItem(
                "Paste Component Values", nullptr, false, canPaste)) {
            before = ops.Capture(*component);
            MeshColliderValue pasted =
                std::any_cast<MeshColliderValue>(componentClipboard);
            pasted.enabled = component->enabled;
            ops.Apply(*component, pasted);
            PushMeshColliderValueCommand(
                *go, ctx, std::string("Paste ") + label,
                before, pasted, ops);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Component"))
            removeRequested = true;
        ImGui::EndPopup();
    }

    if (open) {
        static MeshColliderActiveEdit edits[2];
        MeshColliderActiveEdit& edit = edits[ops.editSlot];
        if (!canRecordUndo)
            edit.active = false;
        ImGui::Spacing();

        if (canRecordUndo)
            before = ops.Capture(*component);
        const ImGuiID activeBefore = ImGui::GetActiveID();
        ops.Draw(*component, *go, ctx.projectRoot);
        const ImGuiID activeAfter = ImGui::GetActiveID();

        if (!canRecordUndo) {
            edit.active = false;
        } else if (!edit.active && activeAfter != 0 && activeAfter != activeBefore) {
            edit.entityId = go->GetID();
            edit.activeId = activeAfter;
            edit.before = before;
            edit.active = true;
        } else if (edit.active && edit.entityId != go->GetID()) {
            if (activeAfter != edit.activeId) edit.active = false;
        } else if (edit.active && activeAfter != edit.activeId) {
            PushMeshColliderValueCommand(
                *go, ctx, std::string("Change ") + label,
                edit.before, ops.Capture(*component), ops);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
            edit.active = false;
        }
        ImGui::Spacing();
    }

    ImGui::PopID();

    if (!removeRequested) return;

    const MeshColliderValue removed = ops.Capture(*component);
    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go->instanceId;
    const auto markDirty = ctx.markSceneDirty;
    const MeshColliderSectionOps* operation = &ops;
    ops.Remove(*go);
    if (canRecordUndo && scene) {
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            std::string("Remove ") + label,
            [scene, instanceId, markDirty, operation]() {
                if (auto* target = scene->FindByGuid(instanceId)) {
                    if (operation->Get(*target))
                        operation->Remove(*target);
                    if (markDirty) markDirty();
                }
            },
            [scene, instanceId, removed, markDirty, operation]() {
                if (auto* target = scene->FindByGuid(instanceId)) {
                    if (!operation->Get(*target)) {
                        auto& restored = operation->Add(*target);
                        operation->Apply(restored, removed);
                    }
                    if (markDirty) markDirty();
                }
            }));
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
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

    DrawMeshColliderComponentSection(
        go, ctx, m_componentClipboard, m_componentClipboardType,
        "Mesh Collider", MESH_COLLIDER_OPS);

    DrawMeshColliderComponentSection(
        go, ctx, m_componentClipboard, m_componentClipboardType,
        "Convex Hull Collider", CONVEX_HULL_COLLIDER_OPS);

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
