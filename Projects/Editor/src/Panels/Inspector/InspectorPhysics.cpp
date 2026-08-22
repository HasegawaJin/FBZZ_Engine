// FBZZ Engine
// InspectorPhysics.cpp | fbzz::editor
// Physics 系 Component の Inspector 描画
#include "InspectorPhysics.hpp"
#include <algorithm>

namespace fbzz::editor {

namespace {

// プリセット適用ボタン。共有アセットを使っていないコライダーの初期値決めに使う。
// WHY ここに出すか: physics::PhysicsMaterial のプリセット (Rubber / Ice / ...) は
//     以前から定義されていたのに Editor から選ぶ手段が無く、実質使われていなかった。
void DrawPhysicsMaterialPresetMenu(physics::PhysicsMaterial& material)
{
    // 選択結果を保持しない「適用するだけ」のコンボ。
    // WHY 現在値を表示しないか: 適用後に値を手で触れるため、プリセット名を出すと
    //     実際の値と食い違ったまま表示が残る。適用の入口としてだけ機能させる。
    if (!ImGui::BeginCombo("Preset", "Apply preset..."))
        return;
    for (int i = 0; i < physics::PhysicsMaterial::PRESET_COUNT; ++i) {
        const char* name = physics::PhysicsMaterial::PresetName(i);
        if (!ImGui::Selectable(name)) continue;
        if (const auto* preset = physics::PhysicsMaterial::PresetAt(i))
            material = *preset;
    }
    ImGui::EndCombo();
}

// go は同じ GameObject の RigidBody を見て density の効き方を注記するために取る。
void DrawColliderCommon(scene::ColliderComponent& col, scene::GameObject& go,
                        const std::string& projectRoot)
{
    widgets::DragVec3("Center", col.center, 0.01f, -1000.0f, 1000.0f);
    ImGui::Checkbox("Is Trigger", &col.isTrigger);

    // 共有 .physmat スロット。割り当てるとインライン編集を閉じる。
    // WHY 変更検知の戻り値を使わないか: 参照がある間はどのみち毎フレーム解決するため、
    //     割り当て直後だけ余分に解決しても意味が無い。解決口を 1 つに絞る。
    widgets::AssetPathField("Physics Material", col.physicsMaterialPath, ".physmat", projectRoot);

    const bool usesSharedAsset = !col.physicsMaterialPath.empty();
    if (usesSharedAsset) {
        // 解決済みの実効値を読み取り専用で見せる。
        // WHY 表示するか: 「このコライダーが結局どんな物性で動くのか」を、
        //     .physmat を開き直さずに確認できるようにする。編集は共有アセット側で行う。
        const bool resolved = col.ResolvePhysicsMaterial();

        // WHY 失敗を明示するか: 解決できなくても col.material には最後に解決できた値
        //     (無ければ既定値) が残り続ける。数値だけ見ても正常時と区別が付かないため、
        //     「アセットを割り当てたのに物理挙動が変わらない」の原因がここだと分からない。
        if (!resolved) {
            ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f },
                               "参照を解決できません。下の値は最後に解決できた値です");
            ImGui::TextDisabled("パスの綴りとアセットの実在を確認してください");
        }

        ImGui::TextDisabled("Restitution %.3f  /  Friction %.3f (static %.3f)  /  Density %.3f",
                            col.material.restitution,
                            col.material.dynamicFriction,
                            col.material.staticFriction,
                            col.material.density);
        if (resolved)
            ImGui::TextDisabled("値の編集は .physmat 側で行う (参照している全コライダーへ反映)");

        // Density は RigidBody の Mass Mode が From Density のときだけ質量へ効く。
        // WHY ここで断るか: .physmat 側で density をいくら大きくしても既定の Manual では
        //     何も起きない。値を触った本人がその場で気付けないと、原因を物理側へ探しに行く。
        if (auto* rb = go.GetComponent<scene::RigidBodyComponent>();
            rb && rb->massMode != scene::MassMode::FromDensity) {
            ImGui::TextDisabled("Density は RigidBody の Mass Mode = From Density でのみ質量に反映されます");
        }
        return;
    }

    DrawPhysicsMaterialPresetMenu(col.material);
    ImGui::DragFloat("Restitution", &col.material.restitution, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Static Friction", &col.material.staticFriction, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Dynamic Friction", &col.material.dynamicFriction, 0.01f, 0.0f, 10.0f);
    ImGui::DragFloat("Density", &col.material.density, 0.01f, 0.0f, 100000.0f);
}

void DrawAabbCollider(scene::AabbColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
    widgets::DragVec3("Size", col.size, 0.01f, 0.001f, 1000.0f);
    // 形状パラメータの反映と姿勢同期は SyncColliderPreview (= ColliderSync) が行う。
    SyncColliderPreview(go, col);
}

void DrawBoxCollider(scene::BoxColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
    widgets::DragVec3("Size", col.size, 0.01f, 0.001f, 1000.0f);
    SyncColliderPreview(go, col);
}

void DrawSphereCollider(scene::SphereColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    SyncColliderPreview(go, col);
}

void DrawCapsuleCollider(scene::CapsuleColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    ImGui::DragFloat("Half Height", &col.halfHeight, 0.01f, 0.001f, 1000.0f);
    SyncColliderPreview(go, col);
}

void DrawCylinderCollider(scene::CylinderColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
    ImGui::DragFloat("Radius", &col.radius, 0.01f, 0.001f, 1000.0f);
    ImGui::DragFloat("Half Height", &col.halfHeight, 0.01f, 0.001f, 1000.0f);
    SyncColliderPreview(go, col);
}

void DrawMeshCollider(scene::MeshColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
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

void DrawTerrainCollider(scene::TerrainColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
    if (!go.GetComponent<scene::TerrainComponent>())
        ImGui::TextColored({ 1.0f, 0.6f, 0.2f, 1.0f }, "TerrainComponent が同じ GO に必要です");
    SyncColliderPreview(go, col);
}

void DrawConvexHullCollider(scene::ConvexHullColliderComponent& col, scene::GameObject& go, const std::string& projectRoot)
{
    DrawColliderCommon(col, go, projectRoot);
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
    // 共有 .physmat の参照も Undo 対象に含める。
    // WHY: 参照だけ戻らないと「Undo したのに物性が元に戻らない」という、
    //      原因が最も分かりにくい壊れ方をする。
    std::string physicsMaterialPath;
    math::Vector3 center;
    std::string meshPath;
    int meshIndex = 0;
    bool isTrigger = false;
    bool useTransformScale = true;
    bool enabled = true;

    // Undo を積むべきかの判定に使う (ComponentSnapshotCompare が検出する)。
    // WHY 必要か: 参照欄をクリックして .physmat を見に行くだけでも ImGui の ActiveID は
    //     動く。それを「編集した」とみなしていたため、中身の変わらない履歴が残っていた。
    // WHY PhysicsMaterial をメンバーごとに比較するか: あちらは物理側の値型で
    //     operator== を持たない。エディタ都合の比較のために物理層へ手を入れない。
    bool operator==(const MeshColliderValue& o) const
    {
        return material.restitution     == o.material.restitution
            && material.staticFriction  == o.material.staticFriction
            && material.dynamicFriction == o.material.dynamicFriction
            && material.density         == o.material.density
            // 合成規則は現在 Inspector から編集できないが、.physmat の割り当てで
            // 差し替わる値なので比較に含めておく (将来 UI を出したときの取りこぼし防止)。
            && material.restitutionCombine == o.material.restitutionCombine
            && material.frictionCombine    == o.material.frictionCombine
            && physicsMaterialPath == o.physicsMaterialPath
            && center.x == o.center.x && center.y == o.center.y && center.z == o.center.z
            && meshPath          == o.meshPath
            && meshIndex         == o.meshIndex
            && isTrigger         == o.isTrigger
            && useTransformScale == o.useTransformScale
            && enabled           == o.enabled;
    }
};

MeshColliderValue CaptureMeshColliderValue(const scene::MeshColliderComponent& c)
{
    return { c.material, c.physicsMaterialPath, c.center, c.meshPath, c.meshIndex,
             c.isTrigger, c.useTransformScale, c.enabled };
}

void ApplyMeshColliderValue(scene::MeshColliderComponent& c, const MeshColliderValue& v)
{
    const bool geomChanged = c.meshPath != v.meshPath
        || c.meshIndex != v.meshIndex
        || c.useTransformScale != v.useTransformScale;
    c.material = v.material;
    c.physicsMaterialPath = v.physicsMaterialPath;
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
    return { c.material, c.physicsMaterialPath, c.center, c.meshPath, c.meshIndex,
             c.isTrigger, c.useTransformScale, c.enabled };
}

void ApplyConvexHullValue(scene::ConvexHullColliderComponent& c, const MeshColliderValue& v)
{
    const bool geomChanged = c.meshPath != v.meshPath
        || c.meshIndex != v.meshIndex
        || c.useTransformScale != v.useTransformScale;
    c.material = v.material;
    c.physicsMaterialPath = v.physicsMaterialPath;
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
        [go](scene::AabbColliderComponent& col, EditorContext& ctx2) {
            DrawAabbCollider(col, *go, ctx2.projectRoot);
        });

    DrawComponentSection<scene::BoxColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Box Collider",
        [go](scene::BoxColliderComponent& col, EditorContext& ctx2) {
            DrawBoxCollider(col, *go, ctx2.projectRoot);
        });

    DrawComponentSection<scene::SphereColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Sphere Collider",
        [go](scene::SphereColliderComponent& col, EditorContext& ctx2) {
            DrawSphereCollider(col, *go, ctx2.projectRoot);
        });

    DrawComponentSection<scene::CapsuleColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Capsule Collider",
        [go](scene::CapsuleColliderComponent& col, EditorContext& ctx2) {
            DrawCapsuleCollider(col, *go, ctx2.projectRoot);
        });

    DrawComponentSection<scene::CylinderColliderComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Cylinder Collider",
        [go](scene::CylinderColliderComponent& col, EditorContext& ctx2) {
            DrawCylinderCollider(col, *go, ctx2.projectRoot);
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
        [go](scene::TerrainColliderComponent& col, EditorContext& ctx2) {
            DrawTerrainCollider(col, *go, ctx2.projectRoot);
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

            // 質量の決め方。From Density は「コライダー体積 × PhysicsMaterial.density」を
            // PhysicsSystem が毎フレーム算出して上書きするため、ここでは手入力させない。
            static constexpr const char* kMassModeLabels[] = { "Manual", "From Density" };
            int massModeIndex = static_cast<int>(rb.massMode);
            if (ImGui::Combo("Mass Mode", &massModeIndex, kMassModeLabels, 2))
                rb.massMode = static_cast<scene::MassMode>(massModeIndex);

            float mass = body.GetMass();
            if (rb.massMode == scene::MassMode::FromDensity) {
                ImGui::BeginDisabled();
                ImGui::DragFloat("Mass", &mass, 0.05f, 0.0f, 100000.0f);
                ImGui::EndDisabled();
                ImGui::TextDisabled("コライダー体積 x 密度から算出 (Play 中も追従)");
            } else if (ImGui::DragFloat("Mass", &mass, 0.05f, 0.0f, 100000.0f)) {
                body.SetMass(mass);
            }

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
            ImGui::Checkbox("Enabled", &cc.enabled);
            static constexpr const char* kGroundingModeLabels[] = {
                "Automatic", "Forced Grounded", "Forced Airborne"
            };
            int groundingMode = static_cast<int>(cc.groundingMode);
            if (ImGui::Combo("Grounding Mode", &groundingMode, kGroundingModeLabels, 3))
                cc.SetGroundingMode(static_cast<scene::CharacterGroundingMode>(
                    std::clamp(groundingMode, 0, 2)));
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
