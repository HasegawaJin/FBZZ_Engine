/// @file    InspectorPhysics.cpp
/// @brief   Physics 系 Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorPhysics.hpp"
#include <algorithm>

namespace fbzz::editor {

namespace {

/// @brief プリセット適用ボタン。共有アセットを使っていないコライダーの初期値決めに使う。
/// @note physics::PhysicsMaterial のプリセット (Rubber / Ice / ...) は以前から定義されていたが
///       Editor から選ぶ手段が無く、実質使われていなかった。
void DrawPhysicsMaterialPresetMenu(physics::PhysicsMaterial& material)
{
    /// @note 選択結果を保持しない「適用するだけ」のコンボ。適用後に値を手で触れるため、プリセット名を
    ///       出すと実際の値と食い違ったまま表示が残る。適用の入口としてだけ機能させる。
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

/// go は同じ GameObject の RigidBody を見て density の効き方を注記するために取る。
void DrawColliderCommon(scene::ColliderComponent& col, scene::GameObject& go,
                        const std::string& projectRoot)
{
    widgets::DragVec3("Center", col.center, 0.01f, -1000.0f, 1000.0f);
    ImGui::Checkbox("Is Trigger", &col.isTrigger);

    /// @note 共有 .physmat スロット。割り当てるとインライン編集を閉じる。変更検知の戻り値は使わない。
    ///       参照がある間はどのみち毎フレーム解決するため、解決口を 1 つに絞る。
    widgets::AssetPathField("Physics Material", col.physicsMaterialPath, ".physmat", projectRoot);

    const bool usesSharedAsset = !col.physicsMaterialPath.empty();
    if (usesSharedAsset) {
        /// @note 解決済みの実効値を読み取り専用で見せる。.physmat を開き直さずに実効物性を
        ///       確認できるようにする。編集は共有アセット側で行う。
        const bool resolved = col.ResolvePhysicsMaterial();

        /// @note 解決できなくても col.material には最後に解決できた値 (無ければ既定値) が残り続け、
        ///       数値だけでは正常時と区別が付かないため、失敗を明示する。
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

        /// @note Density は RigidBody の Mass Mode が From Density のときだけ質量へ効く。既定の
        ///       Manual では density をいくら変えても何も起きないため、ここで断る。
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
    /// @note 形状パラメータの反映と姿勢同期は SyncColliderPreview (= ColliderSync) が行う。
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

/// @brief メッシュ系 Collider の Undo スナップショット型。
/// @note runtime 所有物の unique_ptr<Collider> と ColliderHandle はコピー対象から外し、Undo 時に
///       physics body を不必要に無効化しない。collider は geometry (meshPath/meshIndex/
///       useTransformScale) が変わった場合のみリセットする。
struct MeshColliderValue {
    physics::PhysicsMaterial material;
    /// @note 共有 .physmat の参照も Undo 対象に含める。参照だけ戻らないと
    ///       「Undo したのに物性が元に戻らない」という原因が最も分かりにくい壊れ方をする。
    std::string physicsMaterialPath;
    math::Vector3 center;
    std::string meshPath;
    int meshIndex = 0;
    bool isTrigger = false;
    bool useTransformScale = true;
    bool enabled = true;

    /// @note Undo を積むべきかの判定に使う (ComponentSnapshotCompare が検出する)。参照欄をクリックして
    ///       .physmat を見るだけでも ImGui の ActiveID が動き、それを編集とみなすと中身の変わらない
    ///       履歴が残る。PhysicsMaterial は物理側の値型で operator== を持たないため、ここでメンバーごとに比較する。
    bool operator==(const MeshColliderValue& o) const
    {
        return material.restitution     == o.material.restitution
            && material.staticFriction  == o.material.staticFriction
            && material.dynamicFriction == o.material.dynamicFriction
            && material.density         == o.material.density
            /// @note 合成規則は現在 Inspector から編集できないが、.physmat の割り当てで
            ///       差し替わる値なので比較に含めておく (将来 UI を出したときの取りこぼし防止)。
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

/// @name Joint
/// @note 種別で使うフィールドが入れ替わるので、欄そのものは Reflect() の FieldIf に任せ、ここは
///       «Reflect() では表せないもの» だけを足す。相手を指し忘れた・相手に剛体が無い・距離を
///       自動で採ったといった «張れているか» の状態はフィールド一覧には出ないが «垂れない» の原因になる。

/// Reflect() から欄を起こすリフレクタの下ごしらえ。参照スロットが GameObject 名を
/// 出せるよう、Script の Inspector (InspectorCore) と同じ解決器を繋ぐ。
void ConfigureJointRefReflector(ComponentImGuiReflector& reflector, EditorContext& ctx)
{
    reflector.m_projectRoot = ctx.projectRoot;
    if (!ctx.activeScene) return;

    reflector.m_goNameResolver = [scene = ctx.activeScene](scene::EntityID id) -> std::string {
        auto* target = scene->GetGameObject(id);
        return target ? target->name : "(Missing)";
    };
    reflector.m_goListProvider =
        [scene = ctx.activeScene]() -> std::vector<std::pair<scene::EntityID, std::string>> {
            std::vector<std::pair<scene::EntityID, std::string>> out;
            for (auto& object : scene->GameObjects())
                out.emplace_back(object.GetID(), object.name);
            return out;
        };
    reflector.m_refTypeValidator =
        [scene = ctx.activeScene](scene::EntityID id, const char* typeName) -> bool {
            if (!typeName || !typeName[0]) return true;
            auto* target = scene->GetGameObject(id);
            return target && HasRegisteredComponentByName(*target, typeName);
        };
}

void DrawJointStatus(const scene::JointComponent& joint, scene::GameObject& go, EditorContext& ctx)
{
    if (joint.connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Success));
        ImGui::TextUnformatted("Connected");
        ImGui::PopStyleColor();
        if (joint.UsesDistance()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(distance %.3f m%s)", joint.resolvedDistance,
                                joint.autoDistance ? ", auto" : "");
        }
        return;
    }

    /// @note 張れていない理由を名指しする。Play 前は «まだ物理が回っていない» が普通なので、
    ///       «設定が足りない» と区別できるようにしておく。
    const bool hasSelfBody = go.GetComponent<scene::RigidBodyComponent>() != nullptr;
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
    ImGui::TextUnformatted("Not connected");
    ImGui::PopStyleColor();
    if (!hasSelfBody) {
        ImGui::TextDisabled("この GameObject に Rigid Body がありません");
        return;
    }
    if (joint.type == scene::JointType::Chain) {
        if (joint.chainBodies.empty())
            ImGui::TextDisabled("Chain Bodies に 2 節目以降を並べてください");
        return;
    }
    if (!joint.connectedBody.IsValid() && !joint.connectToParent) {
        ImGui::TextDisabled("Connected Body を指すか Connect To Parent を入れてください");
        return;
    }
    if (ctx.activeScene && joint.connectedBody.IsValid()) {
        scene::GameObject* target = joint.connectedBody.Resolve(*ctx.activeScene);
        if (!target)
            ImGui::TextDisabled("Connected Body の参照先が見つかりません");
        else if (!target->GetComponent<scene::RigidBodyComponent>())
            ImGui::TextDisabled("相手 (%s) に Rigid Body がありません", target->name.c_str());
        return;
    }
    ImGui::TextDisabled("Play 中に PhysicsSystem が張ります");
}

void DrawJoint(scene::JointComponent& joint, scene::GameObject& go, EditorContext& ctx)
{
    DrawJointStatus(joint, go, ctx);
    ImGui::Spacing();

    ComponentImGuiReflector reflector;
    ConfigureJointRefReflector(reflector, ctx);
    joint.Reflect(reflector);
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

            /// @note 質量の決め方。From Density は「コライダー体積 × PhysicsMaterial.density」を
            ///       PhysicsSystem が毎フレーム算出して上書きするため、ここでは手入力させない。
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
            /// @note 流れ (Flow Field / 環境流) との結合係数。0 = 受けない (オプトイン)。
            ///       正本はコンポーネント側で、PhysicsSystem が毎フレーム剛体へ押し込む。
            widgets::RangeField("Flow Coupling", rb.flowCoupling, 0.0f, 20.0f, "%.2f",
                                "流れに引きずられる強さ [1/s]。0 で風も水流も受けない");
            ImGui::Checkbox("Allow Sleeping", &body.m_allowSleeping);
            ImGui::Checkbox("Use CCD", &body.m_useCCD);
            ImGui::DragFloat("CCD Radius", &body.m_ccdRadius, 0.01f, 0.001f, 1000.0f);
            ImGui::DragFloat("Charge", &body.m_charge, 0.01f, -1000.0f, 1000.0f);
            ImGui::Checkbox("Gravity Source", &body.m_isGravitationalSource);
            ImGui::DragFloat("Gravity Mass", &body.m_gravitationalMass, 0.05f, 0.0f, 100000.0f);
        });

    DrawComponentSection<scene::JointComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Joint",
        [go](scene::JointComponent& joint, EditorContext& ctx2) {
            DrawJoint(joint, *go, ctx2);
        });

    DrawComponentSection<scene::VolumeComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Volume",
        [](scene::VolumeComponent& volume, EditorContext&) {
            /// @note 列挙は廃止した Buoyancy (= 2) の枠を空けたままなので、整数をそのまま
            ///       Combo の添字に使えない。並びと型を 1 対 1 の表で持つ。
            static constexpr const char* kVolumeNames[] = {
                "Gravity", "Vortex", "Explosion", "Time Dilation", "Magnetic"
            };
            static constexpr physics::VolumeType kVolumeTypes[] = {
                physics::VolumeType::Gravity,
                physics::VolumeType::Vortex,
                physics::VolumeType::Explosion,
                physics::VolumeType::TimeDilation,
                physics::VolumeType::Magnetic
            };
            static constexpr int kVolumeTypeCount =
                static_cast<int>(sizeof(kVolumeTypes) / sizeof(kVolumeTypes[0]));
            int typeIdx = 0;
            for (int i = 0; i < kVolumeTypeCount; ++i)
                if (kVolumeTypes[i] == volume.type) typeIdx = i;
            if (ImGui::Combo("Type", &typeIdx, kVolumeNames, kVolumeTypeCount))
                volume.type = kVolumeTypes[std::clamp(typeIdx, 0, kVolumeTypeCount - 1)];

            widgets::DragVec3("Gravity", volume.gravity, 0.05f);
            widgets::DragVec3("Magnetic Field", volume.magneticField, 0.05f);
            ImGui::DragFloat("Swirl", &volume.swirlStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Inward", &volume.inwardStrength, 0.05f, 0.0f, 1000.0f);
            ImGui::DragFloat("Lift", &volume.liftStrength, 0.05f, 0.0f, 1000.0f);
            /// @note Drag は撤去した。どの VolumeType も読んでいなかった死んだノブで、
            ///       流れの抵抗は Rigid Body の Flow Coupling が持つ。
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
