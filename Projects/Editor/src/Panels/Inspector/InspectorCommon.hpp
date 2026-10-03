/// @file    InspectorCommon.hpp
/// @brief   Inspector のカテゴリ分割ファイルで共有する描画ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ColliderFit.hpp>
#include <Editor/Util/ComponentDefaults.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/MemoryLeakDiff.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Physics/Layer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LifetimeComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Components/WeatherComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <cstring>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/AtmosphericScatteringComponent.hpp>
#include <Engine/Scene/Components/PostProcessVolumeComponent.hpp>
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
/// @note ComponentUndoCompare の特化で参照する。
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/NavMeshModifierComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Renderer/ColorTemperature.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/RigidBody.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <any>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>


namespace fbzz::editor {

inline bool CanRecordEditorUndo(const EditorContext& ctx)
{
    return ctx.undoStack != nullptr &&
           ctx.undoStack->IsRecordingEnabled();
}

template<typename T>
void PushComponentValueCommand(scene::GameObject& go,
                               EditorContext& ctx,
                               const std::string& description,
                               const T& before,
                               const T& after)
{
    if (!CanRecordEditorUndo(ctx) || !ctx.activeScene) return;

    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;
    auto apply = [scene, instanceId, markDirty](const T& value) {
        if (auto* target = scene->FindByGuid(instanceId)) {
            if (auto* component = target->GetComponent<T>()) {
                *component = value;
                if constexpr (std::is_same_v<T, scene::TerrainComponent>) {
                    component->heightDirty = true;
                    component->splatDirty = true;
                    component->materialParamDirty = true;
                    component->colliderDirty = true;
                } else if constexpr (std::is_same_v<T, scene::WaterComponent>) {
                    component->meshDirty = true;
                    component->foamDirty = true;
                    component->texDirty = true;
                } else if constexpr (std::is_same_v<T, scene::MaterialComponent>) {
                    component->material.reset();
                }
                if (markDirty) markDirty();
            }
        }
    };

    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        description,
        [apply, after]() { apply(after); },
        [apply, before]() { apply(before); }));
}

/// @note 「ユーザーが Inspector で編集しうる値」が変わったかを比較する。
/// @note ActiveID が動いた = 編集した、とみなすとクリックや折りたたみでも履歴が積まれる。
/// @note 構造体全体は比較できない ─ GPU ハンドルや dirty フラグなどのランタイム状態を
/// @note 同じ構造体が持っており、毎フレーム差分ありになって Undo が溢れる。
/// @note 基準は Reflect()。載っていないフィールドは SceneSerializer にも保存されないので、
/// @note 「Reflect() されている = 永続的なユーザー状態」がほぼ成り立つ。
/// @note 例外は Reflect() で表現できず専用コードで読み書きするフィールド (vector<struct> 等)。
/// @note そちらは下で個別に特化し、digest に加えてその配列も比較する。

/// @note Reflect() された値をすべて 1 本の文字列へ落とすリフレクタ。
/// @note 文字列へ落とすのは、型ごとの比較関数を書かずに済み、フィールドの追加・削除にも
/// @note 自動で追従するため。呼ばれるのは 1 操作の終わりだけなので毎フレームのコストにならない。
class ComponentReflectDigest final : public scene::IReflector {
public:
    [[nodiscard]] const std::string& Result() const { return m_out; }

    void Field(const char* name, float& v) override         { Put(name); m_out += std::to_string(v); }
    void Field(const char* name, int& v) override           { Put(name); m_out += std::to_string(v); }
    void Field(const char* name, bool& v) override          { Put(name); m_out += (v ? '1' : '0'); }
    void Field(const char* name, std::string& v) override   { Put(name); m_out += v; }
    void Field(const char* name, math::Vector2& v) override { Put(name); Nums({ v.x, v.y }); }
    void Field(const char* name, math::Vector3& v) override { Put(name); Nums({ v.x, v.y, v.z }); }
    void Field(const char* name, math::Vector4& v) override { Put(name); Nums({ v.x, v.y, v.z, v.w }); }
    void Field(const char* name, math::Quaternion& v) override { Put(name); Nums({ v.x, v.y, v.z, v.w }); }

    /// @note 既定実装が空の入口も埋める。埋め忘れるとその型のフィールドが digest に載らず、
    /// @note 「編集したのに Undo できない」側へ倒れる。
    void Field(const char* name, scene::EntityID& v) override
    {
        Put(name);
        m_out += std::to_string(v.index);
        m_out += ':';
        m_out += std::to_string(v.generation);
    }
    void Field(const char* name, input::KeyCode& v) override
    {
        Put(name); m_out += std::to_string(static_cast<int>(v));
    }
    void Field(const char* name, scene::ParticleCurve& v) override
    {
        Put(name);
        m_out += std::to_string(static_cast<int>(v.interpolation));
        m_out += '|';
        for (uint32_t i = 0; i < v.keyCount && i < v.keys.size(); ++i)
            Nums({ v.keys[i].time, v.keys[i].value });
    }
    void Field(const char* name, scene::ParticleGradient& v) override
    {
        Put(name);
        m_out += std::to_string(static_cast<int>(v.interpolation));
        m_out += ':';
        m_out += std::to_string(static_cast<int>(v.colorSpace));
        m_out += '|';
        for (uint32_t i = 0; i < v.keyCount && i < v.keys.size(); ++i) {
            const math::Vector4& c = v.keys[i].color;
            Nums({ v.keys[i].time, c.x, c.y, c.z, c.w });
        }
    }
    void ListField(const char* name, std::vector<float>& values) override        { Put(name); for (float x : values)  { m_out += std::to_string(x); m_out += ','; } }
    void ListField(const char* name, std::vector<int>& values) override          { Put(name); for (int x : values)    { m_out += std::to_string(x); m_out += ','; } }
    void ListField(const char* name, std::vector<bool>& values) override         { Put(name); for (bool x : values)   { m_out += (x ? '1' : '0'); } }
    void ListField(const char* name, std::vector<std::string>& values) override  { Put(name); for (const auto& x : values) { m_out += x; m_out += ','; } }
    void ListField(const char* name, std::vector<math::Vector2>& values) override { Put(name); for (const auto& x : values) Nums({ x.x, x.y }); }
    void ListField(const char* name, std::vector<math::Vector3>& values) override { Put(name); for (const auto& x : values) Nums({ x.x, x.y, x.z }); }
    void ListField(const char* name, std::vector<math::Vector4>& values) override { Put(name); for (const auto& x : values) Nums({ x.x, x.y, x.z, x.w }); }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        Put(name);
        for (const auto& x : values) { m_out += std::to_string(x.id.index); m_out += ','; }
    }
    void AssetListField(const char* name,
                        std::vector<scene::ScriptAssetReference>& values,
                        scene::ScriptAssetType) override
    {
        Put(name);
        for (const auto& x : values) { m_out += x.guid; m_out += '|'; m_out += x.path; m_out += ','; }
    }

private:
    void Put(const char* name) { m_out += ';'; m_out += name ? name : ""; m_out += '='; }
    void Nums(std::initializer_list<float> values)
    {
        for (float x : values) { m_out += std::to_string(x); m_out += ','; }
    }
    std::string m_out;
};

/// @note component の Reflect() を通した値の digest。
/// @note 非 const 参照を取るのは、Reflect() は値を書き戻す実装 (enum のクランプ等) があり
/// @note const では呼べないため。呼び出し側はどちらも実体を持っているので問題にならない。
template<typename T>
[[nodiscard]] std::string CaptureComponentDigest(T& component)
{
    ComponentReflectDigest digest;
    component.Reflect(digest);
    return digest.Result();
}

/// @note Reflect() がそのコンポーネントの編集可能な状態を完全に覆っていることの宣言。
/// @note 既定は false = 必ず Undo を積む (安全側)。書き忘れても履歴が少し多いだけで済むが、
/// @note 既定を true にすると「編集したのに Undo できない」取り返しのつかない壊れ方になる。
template<typename T>
struct ComponentUndoReflects : std::false_type {};

template<typename T>
struct ComponentUndoCompare {
    static bool UserValuesEqual(T& a, T& b)
    {
        if constexpr (ComponentUndoReflects<T>::value)
            return CaptureComponentDigest(a) == CaptureComponentDigest(b);
        else
            return false;
    }
};

/// @brief Reflect() が編集可能な状態を完全に覆っているコンポーネントの一覧。
/// @note Reflect() に載っていないフィールドは SceneSerializer にも保存されないため、
/// @note この一覧は «専用の保存コードを持たないコンポーネント» と一致する。
#define FBZZ_COMPONENT_UNDO_REFLECTS(Type)                                      \
    template<> struct ComponentUndoReflects<scene::Type> : std::true_type {};

FBZZ_COMPONENT_UNDO_REFLECTS(AabbColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(AnimatorComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(AtmosphericScatteringComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(BoneComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(BoxColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(CapsuleColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(CharacterControllerComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(ConvexHullColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(CylinderColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(DecalComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(EnvironmentLightComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(JointComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(LightComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(MeshColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(MeshTrailComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshAgentComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshModifierComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshOffMeshLinkComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshSensorComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshSurfaceComponent)
/// @note ParticleEmitter は意図的にここへ載せない。保存は ParticleEmitterAssetCodec が担い、
/// @note Reflect() は編集可能な状態の一部 (Trail / Collision / 各カーブ / bursts 等) しか覆わない。
/// @note 宣言すると digest が一致し、編集しても Undo に積まれず «保存されず黙って消える» になる。
/// @note Reflect() が codec と一致したらここへ戻すこと。
FBZZ_COMPONENT_UNDO_REFLECTS(FlowField)
FBZZ_COMPONENT_UNDO_REFLECTS(PostProcessVolumeComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(ReflectionProbeComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(LightProbeVolumeComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(RigidBodyComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(SkyRenderer)
FBZZ_COMPONENT_UNDO_REFLECTS(SphereColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(SunMoonRenderer)
FBZZ_COMPONENT_UNDO_REFLECTS(TerrainColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(TrailComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(VolumeComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(VolumetricCloudComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(WaterComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(WeatherComponent)

#undef FBZZ_COMPONENT_UNDO_REFLECTS

/// @note いずれも「digest + Reflect に載らない編集対象」を比較する。
/// @note digest 側でカバーされるフィールドは重複して書かない (増減に自動追従させる)。

/// @note 固定長配列・vector<struct> の比較ヘルパー。
template<typename T, typename Equal>
[[nodiscard]] bool RangesEqual(const std::vector<T>& a, const std::vector<T>& b, Equal equal)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!equal(a[i], b[i])) return false;
    return true;
}

/// @note DrawComponentSectionCustom (専用 Snapshot を使うコンポーネント) の変更判定。
/// @note Snapshot が operator== を持っていればそれを使い、無ければ必ず積む。
/// @note Snapshot 型は .cpp の無名名前空間にあり明示的特化を書けないので、== の検出で拾う。
template<typename Snapshot>
struct ComponentSnapshotCompare {
    static bool Equal(const Snapshot& a, const Snapshot& b)
    {
        if constexpr (requires { a == b; }) return a == b;
        else return false;
    }
};

/// @note マテリアルスロット 1 つぶんの、ユーザーが編集しうる値。
/// @note paramOverrides / textureOverrides / 各 override フラグはスクリプトが実行中に
/// @note 書き込むランタイム専用の状態なので比較に含めない。
inline bool MaterialSlotUserValuesEqual(const scene::MaterialSlot& a,
                                        const scene::MaterialSlot& b)
{
    return a.materialPath == b.materialPath && a.visible == b.visible;
}

/// @note Renderer 系は mesh / model が生ポインタで Reflect に載らない (パスから解決される)。
/// @note Inspector でメッシュを差し替えるとポインタだけが変わる瞬間があるため明示的に見る。
/// @note lodVisible は LODSystem が毎フレーム書き換えるランタイム値なので除く。
template<>
struct ComponentUndoCompare<scene::MeshRenderer> {
    static bool UserValuesEqual(scene::MeshRenderer& a, scene::MeshRenderer& b)
    {
        return a.mesh == b.mesh
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note nodeEntities / morphWeights / 各頂点バッファ / gpuSkinnedThisFrame は
/// @note AnimatorSystem と SkinningComputePass が毎フレーム書き換えるため除く。
/// @note submeshIndices は配置時に決まり Reflect には載らないので明示的に見る。
template<>
struct ComponentUndoCompare<scene::SkinnedMeshRenderer> {
    static bool UserValuesEqual(scene::SkinnedMeshRenderer& a, scene::SkinnedMeshRenderer& b)
    {
        return a.model          == b.model
            && a.submeshIndices == b.submeshIndices
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note MaterialComponent::Reflect はスロット 0 の materialPath までしか載せない。
/// @note extraSlots (submesh 1 以降) は SceneSerializer が専用コードで読み書きする。
template<>
struct ComponentUndoCompare<scene::MaterialComponent> {
    static bool UserValuesEqual(scene::MaterialComponent& a, scene::MaterialComponent& b)
    {
        if (a.SlotCount() != b.SlotCount()) return false;
        for (size_t i = 0; i < a.SlotCount(); ++i)
            if (!MaterialSlotUserValuesEqual(a.RawSlotAt(i), b.RawSlotAt(i)))
                return false;
        return CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note layerCullDistances は固定長配列で IReflector に対応する Field が無い。
template<>
struct ComponentUndoCompare<scene::CameraComponent> {
    static bool UserValuesEqual(scene::CameraComponent& a, scene::CameraComponent& b)
    {
        for (size_t i = 0; i < std::size(a.layerCullDistances); ++i)
            if (a.layerCullDistances[i] != b.layerCullDistances[i]) return false;
        return CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note levels は入れ子 vector のため Reflect に載らない。
/// @note 比較するのは Inspector で編集できる閾値と Renderer 参照の並びだけ。
/// @note (LODRendererReference::entity は instanceId から解決されるランタイム値)
template<>
struct ComponentUndoCompare<scene::LODGroupComponent> {
    static bool UserValuesEqual(scene::LODGroupComponent& a, scene::LODGroupComponent& b)
    {
        const auto levelEqual = [](const scene::LODLevel& x, const scene::LODLevel& y) {
            if (x.screenRelativeHeight != y.screenRelativeHeight) return false;
            return RangesEqual(x.renderers, y.renderers,
                [](const scene::LODRendererReference& p, const scene::LODRendererReference& q) {
                    return p.instanceId == q.instanceId;
                });
        };
        return RangesEqual(a.levels, b.levels, levelEqual)
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note waypoints と対応する per-waypoint 配列は Reflect に載らない。
template<>
struct ComponentUndoCompare<scene::NavMeshPatrolComponent> {
    static bool UserValuesEqual(scene::NavMeshPatrolComponent& a,
                                scene::NavMeshPatrolComponent& b)
    {
        const auto vec3Equal = [](const math::Vector3& x, const math::Vector3& y) {
            return x.x == y.x && x.y == y.y && x.z == y.z;
        };
        const auto floatEqual = [](float x, float y) { return x == y; };
        return RangesEqual(a.waypoints, b.waypoints, vec3Equal)
            && RangesEqual(a.waypointWaitTimes, b.waypointWaitTimes, floatEqual)
            && RangesEqual(a.waypointSpeeds, b.waypointSpeeds, floatEqual)
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @brief Terrain の Inspector 編集が Undo に積むべき差分か判定する。
/// @note layerMaterials は string 配列のため Reflect に載らない。層の削除・並べ替えと «Clear Holes» は
/// @note splat / holeData を書き換えるので比較に含める。heightData は地形ツールが専用の Undo を持つので除く。
template<>
struct ComponentUndoCompare<scene::TerrainComponent> {
    static bool UserValuesEqual(scene::TerrainComponent& a, scene::TerrainComponent& b)
    {
        return a.layerMaterials == b.layerMaterials
            && a.splatIndices == b.splatIndices
            && a.splatWeights == b.splatWeights
            && a.holeData == b.holeData
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note (IK チェーン編集、Terrain Grid の生成) が編集し、それぞれが自前の Undo を持つ。
/// @note 要素数だけを見て、追加・削除は取りこぼさない。
template<>
struct ComponentUndoCompare<scene::IKSolverComponent> {
    static bool UserValuesEqual(scene::IKSolverComponent& a, scene::IKSolverComponent& b)
    {
        /// @note IKChain は 15 個以上のフィールドを持つが、Inspector で 1 つ変えるたびに
        /// @note Undo を積むべきなので主要な編集対象を見る。チェーン数の増減も拾う。
        const auto chainEqual = [](const scene::IKChain& x, const scene::IKChain& y) {
            return x.type       == y.type
                && x.enabled    == y.enabled
                && x.weight     == y.weight
                && x.order      == y.order
                && x.boneNames  == y.boneNames
                && x.targetGuid == y.targetGuid
                && x.poleGuid   == y.poleGuid
                && x.maxExtension        == y.maxExtension
                && x.softness            == y.softness
                && x.minBendAngleDegrees == y.minBendAngleDegrees
                && x.maxBendAngleDegrees == y.maxBendAngleDegrees
                && x.autoPole            == y.autoPole;
        };
        return RangesEqual(a.chains, b.chains, chainEqual)
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note cells / cellInstanceIds はグリッド生成の結果で、Inspector から直接は編集しない。
template<>
struct ComponentUndoCompare<scene::TerrainGridComponent> {
    static bool UserValuesEqual(scene::TerrainGridComponent& a, scene::TerrainGridComponent& b)
    {
        return a.cellInstanceIds == b.cellInstanceIds
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

/// @note コンポーネント編集の開始状態を保持する。
/// @note MSVC 14.51 は関数テンプレート内の依存型を持つローカル構造体で ICE するため、
/// @note 状態型を名前空間スコープへ分離してテンプレートのインスタンス化を単純化する。
template<typename T>
struct ComponentActiveEdit {
    scene::EntityID id;
    ImGuiID activeId = 0;
    T before{};
    bool active = false;
};

/// @note DrawComponentSectionCustom 用 — Snapshot 型を T と分離した版。
/// @note unique_ptr を持つコンポーネントでは T をそのままスナップショットに使えないため分離する。
/// @note (T, Snapshot) ペアごとに static スロットが生成されるため MeshCollider / ConvexHull 分離を保証する。
template<typename T, typename Snapshot>
struct ComponentActiveEditCustom {
    scene::EntityID entityId{};
    ImGuiID activeId = 0;
    Snapshot before{};
    bool active = false;
};

/// @note 型付き描画関数を、Undo 処理が受け取る型消去済みコールバックへ橋渡しする。
/// @note MSVC 14.51 の ICE を避けるため、状態保持とスナップショットを行う重い関数から
/// @note ラムダ固有型 DrawFn のテンプレート依存を分離する。
template<typename T, typename DrawFn>
void InvokeComponentDraw(T& component, EditorContext& ctx, void* drawFn)
{
    (*static_cast<DrawFn*>(drawFn))(component, ctx);
}

/// @note コンポーネント内部の ImGui 編集を ActiveId の開始から解放まで1操作として記録する。
/// @note 各 Drag/Slider を個別対応すると記録漏れが生じるため、共通セクションで
/// @note 編集前後のコンポーネント全体をスナップショットする。
template<typename T>
void DrawGenericUndoableComponentBody(scene::GameObject& go,
                                      EditorContext& ctx,
                                      const char* label,
                                      T& component,
                                      void (*drawFn)(T&, EditorContext&, void*),
                                      void* drawFnData)
{
    static ComponentActiveEdit<T> edit;

    if (!CanRecordEditorUndo(ctx)) {
        edit.active = false;
        drawFn(component, ctx, drawFnData);
        return;
    }

    const T beforeDraw = component;
    const ImGuiID activeBefore = ImGui::GetActiveID();
    drawFn(component, ctx, drawFnData);
    const ImGuiID activeAfter = ImGui::GetActiveID();

    if (!edit.active && activeAfter != 0 && activeAfter != activeBefore) {
        edit.id = go.GetID();
        edit.activeId = activeAfter;
        edit.before = beforeDraw;
        edit.active = true;
        return;
    }

    if (edit.active && edit.id != go.GetID()) {
        if (activeAfter != edit.activeId) edit.active = false;
        return;
    }
    if (!edit.active || activeAfter == edit.activeId) return;

    /// @note 値が動いていない操作 (参照欄のクリック・折りたたみの開閉など) では積まない。
    if (ComponentUndoCompare<T>::UserValuesEqual(edit.before, component)) {
        edit.active = false;
        return;
    }

    PushComponentValueCommand(
        go, ctx, std::string("Change ") + label, edit.before, component);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    edit.active = false;
}

/// @note MaterialComponent は参照先コンポーネントと MaterialAsset 本体を同じ UI で編集する。
/// @note 汎用スナップショットを使うと Asset 編集時のキャッシュ reset まで別コマンドになり、
/// @note MaterialAsset 側の Undo と二重に履歴へ積まれるため、参照パス変更だけを記録する。
template<typename DrawFn>
void DrawUndoableComponentBody(scene::GameObject& go,
                               EditorContext& ctx,
                               const char* label,
                               scene::MaterialComponent& component,
                               DrawFn drawFn)
{
    struct ActiveEdit {
        scene::EntityID id;
        ImGuiID activeId = 0;
        scene::MaterialComponent before;
        bool active = false;
    };
    static ActiveEdit edit;

    if (!CanRecordEditorUndo(ctx)) {
        edit.active = false;
        drawFn(component, ctx);
        return;
    }

    const scene::MaterialComponent beforeDraw = component;
    const ImGuiID activeBefore = ImGui::GetActiveID();
    drawFn(component, ctx);
    const ImGuiID activeAfter = ImGui::GetActiveID();

    if (!edit.active && activeAfter != 0 && activeAfter != activeBefore) {
        edit.id = go.GetID();
        edit.activeId = activeAfter;
        edit.before = beforeDraw;
        edit.active = true;
        return;
    }
    if (edit.active && edit.id != go.GetID()) {
        if (activeAfter != edit.activeId) edit.active = false;
        return;
    }
    if (!edit.active || activeAfter == edit.activeId) return;

    /// @note 全スロットの割り当てを Undo 対象にする。判定は汎用側と同じ
    /// @note ComponentUndoCompare を使う (以前はここに同じ比較を手書きで重複させていた)。
    if (!ComponentUndoCompare<scene::MaterialComponent>::UserValuesEqual(
            edit.before, component)) {
        PushComponentValueCommand(
            go, ctx, std::string("Change ") + label, edit.before, component);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }
    edit.active = false;
}

/// @note RigidBodyComponent は shared_ptr の先に編集値を持つため、物理ボディ本体も複製する。
/// @note Component の浅いコピーだけでは before/after が同じ RigidBody を参照し、
/// @note Mass や Gravity Scale の Undo が実質的に何も戻さないため。
template<typename DrawFn>
void DrawUndoableComponentBody(scene::GameObject& go,
                               EditorContext& ctx,
                               const char* label,
                               scene::RigidBodyComponent& component,
                               DrawFn drawFn)
{
    struct Snapshot {
        scene::RigidBodyComponent component;
        physics::RigidBody body;
        bool hasBody = false;
    };
    struct ActiveEdit {
        scene::EntityID id;
        ImGuiID activeId = 0;
        Snapshot before;
        bool active = false;
    };
    static ActiveEdit edit;

    if (!CanRecordEditorUndo(ctx)) {
        edit.active = false;
        drawFn(component, ctx);
        return;
    }

    auto capture = [](const scene::RigidBodyComponent& value) {
        Snapshot snapshot;
        snapshot.component = value;
        snapshot.hasBody = value.rigidBody != nullptr;
        if (value.rigidBody) snapshot.body = *value.rigidBody;
        return snapshot;
    };

    const Snapshot beforeDraw = capture(component);
    const ImGuiID activeBefore = ImGui::GetActiveID();
    drawFn(component, ctx);
    const ImGuiID activeAfter = ImGui::GetActiveID();

    if (!edit.active && activeAfter != 0 && activeAfter != activeBefore) {
        edit.id = go.GetID();
        edit.activeId = activeAfter;
        edit.before = beforeDraw;
        edit.active = true;
        return;
    }
    if (edit.active && edit.id != go.GetID()) {
        if (activeAfter != edit.activeId) edit.active = false;
        return;
    }
    if (!edit.active || activeAfter == edit.activeId) return;

    const Snapshot after = capture(component);

    /// @note 値が動いていない操作 (参照欄のクリック等) では積まない。
    /// @note RigidBodyComponent::Reflect は body の先まで読み書きし、Snapshot も body を
    /// @note 深いコピーで持つので、component の digest だけで足りる。
    if (edit.before.hasBody == after.hasBody &&
        ComponentUndoCompare<scene::RigidBodyComponent>::UserValuesEqual(
            edit.before.component, component)) {
        edit.active = false;
        return;
    }

    if (ctx.undoStack && ctx.activeScene) {
        scene::Scene* scene = ctx.activeScene;
        const std::string instanceId = go.instanceId;
        const auto markDirty = ctx.markSceneDirty;
        auto apply = [scene, instanceId, markDirty](const Snapshot& snapshot) {
            if (auto* target = scene->FindByGuid(instanceId)) {
                if (auto* rigidBody = target->GetComponent<scene::RigidBodyComponent>()) {
                    rigidBody->enabled = snapshot.component.enabled;
                    rigidBody->bodyHandle = snapshot.component.bodyHandle;
                    if (snapshot.hasBody) {
                        if (!rigidBody->rigidBody)
                            rigidBody->rigidBody = std::make_unique<physics::RigidBody>();
                        *rigidBody->rigidBody = snapshot.body;
                    } else {
                        rigidBody->rigidBody.reset();
                    }
                    if (markDirty) markDirty();
                }
            }
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            std::string("Change ") + label,
            [apply, after]() { apply(after); },
            [apply, before = edit.before]() { apply(before); }));
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    edit.active = false;
}


/// @note Hierarchy パネルからのドラッグ＆ドロップを受け取り、ドロップされた GameObject を返す。
/// @note IK Solver の Bone 名・Target 名フィールドに Hierarchy から直接ドロップできるようにする。
/// @note nullptr の場合はドロップなし (BeginDragDropTarget が false を返すか payload 不正)。
inline scene::GameObject* AcceptHierarchyDrop(scene::Scene* scene)
{
    if (!ImGui::BeginDragDropTarget()) return nullptr;
    scene::GameObject* result = nullptr;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY")) {
        if (p->DataSize == sizeof(scene::EntityID) && scene) {
            scene::EntityID id;
            std::memcpy(&id, p->Data, sizeof(id));
            result = scene->GetGameObject(id);
        }
    }
    ImGui::EndDragDropTarget();
    return result;
}

/// @note カテゴリ別のアクセント色。コンポーネントカードの左帯に使う。
/// @note Inspector は 10 枚以上のカードが縦に積まれるため、名前を読まないと種類が分からない。
/// @note 系統ごとに色を割り当てておけば、スクロール中でも «緑の帯 = 物理» で目的地を拾える。
inline ImU32 ComponentCategoryAccent(scene::ComponentCategory category)
{
    using Category = scene::ComponentCategory;
    switch (category) {
    case Category::Rendering:   return IM_COL32( 90, 160, 245, 255);
    case Category::Lighting:    return IM_COL32(245, 200,  80, 255);
    case Category::Physics:     return IM_COL32(120, 205, 140, 255);
    case Category::Animation:   return IM_COL32(210, 130, 235, 255);
    case Category::Audio:       return IM_COL32( 90, 210, 205, 255);
    case Category::Effects:     return IM_COL32(240, 140, 180, 255);
    case Category::Environment: return IM_COL32(140, 200, 235, 255);
    case Category::Navigation:  return IM_COL32(150, 190, 110, 255);
    case Category::Terrain:     return IM_COL32(200, 165, 110, 255);
    case Category::UI:          return IM_COL32(235, 165,  95, 255);
    case Category::Misc:        return IM_COL32(150, 155, 170, 255);
    case Category::Internal:    return IM_COL32(120, 125, 140, 255);
    }
    return IM_COL32(150, 155, 170, 255);
}

/// @note 登録テーブルから型 → カテゴリを引く (未登録は Misc)。
/// @note テンプレート側で ForEachRegisteredComponent を回すと実体化が型数の 2 乗になる。
/// @note テーブル化を非テンプレート関数へ閉じ込め、実体化を 1 回に抑える。
inline scene::ComponentCategory LookupComponentCategory(const std::type_info& type)
{
    static const std::vector<std::pair<std::type_index, scene::ComponentCategory>> table = []() {
        std::vector<std::pair<std::type_index, scene::ComponentCategory>> out;
        scene::ForEachRegisteredComponent([&]<typename U, typename Registration>() {
            out.emplace_back(std::type_index(typeid(U)), Registration::category);
        });
        return out;
    }();

    const std::type_index key(type);
    for (const auto& [registered, category] : table)
        if (registered == key) return category;
    return scene::ComponentCategory::Misc;
}

/// @note 型ごとの帯色。引き当て結果は型ごとの static に畳むので、毎フレームの検索にはならない。
template<typename T>
inline ImU32 ComponentAccent()
{
    static const ImU32 accent = ComponentCategoryAccent(LookupComponentCategory(typeid(T)));
    return accent;
}

/// @note コンポーネントの「有効フラグ」の置き場を吸収する。
/// @note 大半は直下の bool enabled だが、オーサリング値を分けた型 (ParticleEmitter) は
/// @note settings.enabled に置く。直下しか見ないと «Inspector から無効化も再有効化もできない»。
template<typename T>
[[nodiscard]] constexpr bool ComponentHasEnabled()
{
    return requires(T& value) { static_cast<bool&>(value.enabled); }
        || requires(T& value) { static_cast<bool&>(value.settings.enabled); };
}

template<typename T>
[[nodiscard]] inline bool& ComponentEnabledFlag(T& value)
{
    if constexpr (requires { static_cast<bool&>(value.enabled); })
        return value.enabled;
    else
        return value.settings.enabled;
}

/// @note 既存のカテゴリ別 Inspector を一度収集し、GameObject が持つ順序で再生するための一時バッファ。
/// @note 描画要求だけを遅延させれば、既存の Component 固有 UI と Undo 実装に触れずに
/// @note カードの並び順だけを差し替えられる。
struct InspectorComponentDrawCollector {
    struct Request {
        std::string key;
        std::function<void()> draw;
    };

    scene::GameObject* gameObject = nullptr;
    EditorSceneState* editorState = nullptr;
    std::vector<Request> requests;
    bool drawing = false;
    /// @note 収集が「この GameObject の全 Component」を網羅しているときだけ true。
    /// @note Map モードの絞り込み表示では地形系しか収集されない。そこで残骸掃除まで
    /// @note 走らせると、表示していないだけの Component の並び順を消してしまう。
    bool complete = false;

    void Add(std::string key, std::function<void()> draw)
    {
        requests.push_back({ std::move(key), std::move(draw) });
    }

    void DrawInOrder()
    {
        if (!gameObject) return;

        /// @note 今フレーム実在するキーだけを保存対象に残す (削除された Component の残骸掃除)。
        if (complete && editorState) {
            std::vector<std::string> presentKeys;
            presentKeys.reserve(requests.size());
            for (const Request& request : requests) presentKeys.push_back(request.key);
            editorState->PruneComponentOrder(gameObject->instanceId, presentKeys);
        }

        std::vector<bool> drawn(requests.size(), false);
        const auto drawRequest = [&](size_t index) {
            if (index >= requests.size() || drawn[index]) return;
            drawn[index] = true;
            drawing = true;
            requests[index].draw();
            drawing = false;
        };

        /// @note 描画中のドロップで順序が変更されてもイテレータを無効化しないよう、
        /// @note 並び順はフレーム開始時点のスナップショットを使う。
        const std::vector<std::string> order = editorState
            ? editorState->GetComponentOrder(gameObject->instanceId)
            : std::vector<std::string>{};
        /// @note 保存済み順序に存在する要求を先に描き、後から追加された Component は末尾へ置く。
        for (const std::string& key : order) {
            for (size_t i = 0; i < requests.size(); ++i)
                if (requests[i].key == key) drawRequest(i);
        }
        for (size_t i = 0; i < requests.size(); ++i)
            drawRequest(i);
    }
};

/// @note Component カードの移動を 1 回の Undo 操作として記録する。
/// @note ドロップ後に別のカードを追加・削除しても GameObject* を保持し続けないよう、
/// @note Undo/Redo 時は instanceId から対象を引き直す。
inline void MoveInspectorComponentWithUndo(scene::GameObject& go,
                                            EditorContext& ctx,
                                            std::string_view draggedKey,
                                            std::string_view targetKey,
                                            bool insertAfter)
{
    const std::vector<std::string> before =
        ctx.editorSceneState.GetComponentOrder(go.instanceId);
    if (!ctx.editorSceneState.MoveComponentOrder(
            go.instanceId, draggedKey, targetKey, insertAfter)) return;
    const std::vector<std::string> after =
        ctx.editorSceneState.GetComponentOrder(go.instanceId);
    if (before == after) return;

    if (CanRecordEditorUndo(ctx) && ctx.activeScene) {
        EditorSceneState* editorState = &ctx.editorSceneState;
        const std::string instanceId = go.instanceId;
        const auto markDirty = ctx.markSceneDirty;
        const auto apply = [editorState, instanceId, markDirty](
                               const std::vector<std::string>& order) {
            editorState->SetComponentOrder(instanceId, order);
            if (markDirty) markDirty();
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Reorder Components",
            [apply, after]()  { apply(after); },
            [apply, before]() { apply(before); }));
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

/// @note エンジン Component カード共通の並び替え指定。
/// @note まとめるのは、scope 文字列とドロップ処理を各カードで書き写すと、片方だけ直したときに
/// @note «一部のカードだけ並び替えできない / 別リストへ落とせてしまう» ズレが生まれるため。
inline widgets::ComponentReorderTarget MakeComponentReorderTarget(
    scene::GameObject* go, EditorContext& ctx, const char* label)
{
    widgets::ComponentReorderTarget reorder;
    reorder.scope  = "COMPONENT";
    reorder.onDrop = [go, &ctx, label](std::string_view draggedKey, bool insertAfter) {
        MoveInspectorComponentWithUndo(*go, ctx, draggedKey, label, insertAfter);
    };
    return reorder;
}

inline void DrawPasteComponentAsNewMenuItem(scene::GameObject& go,
                                            EditorContext& ctx,
                                            const std::any& clipboard,
                                            const std::type_info* clipboardType);

/// @brief "Remove Component" 項目。外すと壊れる相手が居れば淡色にしてツールチップで名指しする。
/// @return 押されたら true。
inline bool DrawRemoveComponentMenuItem(scene::GameObject& go, const std::type_info& type)
{
    const std::string blocker = FindComponentRemovalBlocker(go, type);
    const bool clicked = ImGui::MenuItem("Remove Component", nullptr, false, blocker.empty());
    if (!blocker.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Required by %s", blocker.c_str());
    return clicked;
}

/// @brief 内部型 (addable = false) のカードでも Remove を出す型。
/// @note ProceduralMesh はスクリプトの mesh.Apply が付け直すので外しても壊れない。Bone は骨格が壊れる。
template<typename T>
[[nodiscard]] constexpr bool ComponentCardAllowsRemove()
{
    return ComponentAddableTrait<T>::value || std::is_same_v<T, scene::ProceduralMeshComponent>;
}

/// @brief 1 コンポーネントぶんのカード (ヘッダー・メニュー・Undo 付き本文) を描く。
/// @param label 未登録型の見出し。登録型は ComponentDisplayName が正本で、並び順キーもそれを使う。
/// @note 内部型は Reset / Paste を出さず、本文は読み取り専用として Undo を追跡しない。
template<typename T, typename DrawFn>
void DrawComponentSection(scene::GameObject* go,
                          EditorContext& ctx,
                          std::any& compClipboard,
                          const std::type_info*& compClipboardType,
                          const char* label,
                          DrawFn drawFn)
{
    auto* comp = go->GetComponent<T>();
    if (!comp) return;
    label = ComponentDisplayName<T>(label);

    if (ctx.inspectorComponentCollector && !ctx.inspectorComponentCollector->drawing) {
        const std::string key = label;
        ctx.editorSceneState.EnsureComponentOrder(go->instanceId, key);
        auto* collector = ctx.inspectorComponentCollector;
        collector->Add(key, [collector, go, &ctx, &compClipboard, &compClipboardType,
                             key, drawFn]() {
            collector->drawing = true;
            DrawComponentSection<T>(go, ctx, compClipboard, compClipboardType,
                                    key.c_str(), drawFn);
            collector->drawing = false;
        });
        return;
    }

    ImGui::PushID(label);

    const ImU32 accent = ComponentAccent<T>();
    widgets::ComponentHeaderResult header;
    constexpr bool internalType = !ComponentAddableTrait<T>::value;

    /// @note Bone のような enabled を持たない型はチェックを出さない。置き場の差は ComponentEnabledFlag が吸収する。
    constexpr bool hasEnabled = ComponentHasEnabled<T>();
    if constexpr (hasEnabled) {
        header = widgets::ComponentHeader(
            label, accent, &ComponentEnabledFlag(*comp), true,
            MakeComponentReorderTarget(go, ctx, label));
        if (header.enabledChanged) {
            /// @note 変わったのはフラグだけなので、変化後から直前を組む (毎フレームの全体コピーを避ける)。
            T beforeEnabled = *comp;
            ComponentEnabledFlag(beforeEnabled) = !ComponentEnabledFlag(*comp);
            PushComponentValueCommand(
                *go, ctx, std::string("Toggle ") + label, beforeEnabled, *comp);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
    } else {
        header = widgets::ComponentHeader(
            label, accent, nullptr, true,
            MakeComponentReorderTarget(go, ctx, label));
    }

    const bool open = header.open;
    if (header.menuClicked)
        ImGui::OpenPopup("##comp_opts");

    bool removeRequested = false;
    if (ImGui::BeginPopup("##comp_opts")) {
        if (!internalType && ImGui::MenuItem("Reset")) {
            const T before = *comp;
            /// @note Add Component と同じ既定値へ戻す (T{} だと RigidBody の本体や Terrain の格子が消える)。
            T after = MakeDefaultComponent<T>(*go);
            CopyComponentEnabledFlag(before, after);
            *comp = after;
            PushComponentValueCommand(
                *go, ctx, std::string("Reset ") + label, before, *comp);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        if (!internalType) ImGui::Separator();
        if (ImGui::MenuItem("Copy Component"))
        {
            compClipboard     = *comp;
            compClipboardType = &typeid(T);
        }
        const bool canPaste = !internalType && compClipboardType && *compClipboardType == typeid(T);
        if (!internalType && ImGui::MenuItem("Paste Component Values", nullptr, false, canPaste))
        {
            const T before = *comp;
            if constexpr (hasEnabled) {
                const bool wasEnabled = ComponentEnabledFlag(*comp);
                *comp = std::any_cast<T>(compClipboard);
                ComponentEnabledFlag(*comp) = wasEnabled;
            } else {
                *comp = std::any_cast<T>(compClipboard);
            }
            PushComponentValueCommand(
                *go, ctx, std::string("Paste ") + label, before, *comp);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        DrawPasteComponentAsNewMenuItem(*go, ctx, compClipboard, compClipboardType);
        if constexpr (ComponentCardAllowsRemove<T>()) {
            ImGui::Separator();
            if (DrawRemoveComponentMenuItem(*go, typeid(T)))
                removeRequested = true;
        }
        ImGui::EndPopup();
    }

    if (open) {
        const widgets::ComponentBodyScope body = widgets::BeginComponentBody(header, accent);
        ImGui::Spacing();
        if constexpr (internalType) {
            drawFn(*comp, ctx);
        } else if constexpr (std::is_same_v<T, scene::MaterialComponent>
                          || std::is_same_v<T, scene::RigidBodyComponent>) {
            DrawUndoableComponentBody(*go, ctx, label, *comp, drawFn);
        } else {
            DrawGenericUndoableComponentBody(
                *go,
                ctx,
                label,
                *comp,
                &InvokeComponentDraw<T, DrawFn>,
                &drawFn);
        }
        ImGui::Spacing();
        widgets::EndComponentBody(body);
    }
    /// @note カード同士の間隔。詰まっていると帯があっても切れ目が読めない。
    ImGui::Spacing();

    ImGui::PopID();

    if (removeRequested) {
        /// @note 外す直前を基準に取り、数フレーム後の残りを出す («外したのに減らない» をその場で見せる)。
        if (ctx.memoryLeakDiff != nullptr && ctx.resources != nullptr) {
            ctx.memoryLeakDiff->CaptureBaseline(*ctx.resources, std::string("Before remove ") + label);
            ctx.memoryLeakDiff->ScheduleCompare(3, std::string("Remove ") + label);
        }
        /// @note RemoveComponent は GPU リソースを返すので、やり直し用のコピーはハンドルを消しておく (再利用された枠を掴まない)。
        T removed = *comp;
        scene::ClearComponentGpuHandles(removed);
        scene::Scene* scene = ctx.activeScene;
        const std::string instanceId = go->instanceId;
        const auto markDirty = ctx.markSceneDirty;
        go->RemoveComponent<T>();
        if (CanRecordEditorUndo(ctx) && scene) {
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                std::string("Remove ") + label,
                [scene, instanceId, markDirty]() {
                    if (auto* target = scene->FindByGuid(instanceId)) {
                        if (target->GetComponent<T>())
                            target->RemoveComponent<T>();
                        if (markDirty) markDirty();
                    }
                },
                [scene, instanceId, removed, markDirty]() {
                    if (auto* target = scene->FindByGuid(instanceId)) {
                        if (!target->GetComponent<T>())
                            target->AddComponent<T>(removed);
                        if (markDirty) markDirty();
                    }
                }));
        }
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }
}

/// @note EngineコンポーネントのReflect()からInspector本体を自動生成するReflector。
/// @note enabledはDrawComponentSectionの共通ヘッダーがUndo付きで描画するため、
/// @note Reflect()内の同名フィールドだけを省き、二重表示を防ぐ。
struct ComponentImGuiReflector final : ImGuiReflector {
    void Field(const char* name, bool& value) override
    {
        if (std::strcmp(name, "enabled") == 0) return;
        ImGuiReflector::Field(name, value);
    }
};

/// @note Reflect()を持つコピー可能コンポーネントを、共通のヘッダー・Undo・本文描画へ接続する。
template<typename T>
void DrawReflectedComponentSection(scene::GameObject* go,
                                   EditorContext& ctx,
                                   std::any& compClipboard,
                                   const std::type_info*& compClipboardType,
                                   const char* label)
{
    DrawComponentSection<T>(
        go, ctx, compClipboard, compClipboardType, label,
        [](T& component, EditorContext& editorContext) {
            ComponentImGuiReflector reflector;
            reflector.m_projectRoot = editorContext.projectRoot;
            component.Reflect(reflector);
        });
}

/// @note RegistryカテゴリをEditor表示名へ変換する。
inline const char* ComponentCategoryLabel(scene::ComponentCategory category)
{
    using Category = scene::ComponentCategory;
    switch (category) {
    case Category::Rendering:   return "Rendering";
    case Category::Lighting:    return "Lighting";
    case Category::Physics:     return "Physics";
    case Category::Animation:   return "Animation";
    case Category::Audio:       return "Audio";
    case Category::Effects:     return "Effects";
    case Category::Environment: return "Environment";
    case Category::Navigation:  return "Navigation";
    case Category::Terrain:     return "Terrain & Water";
    case Category::UI:          return "UI";
    case Category::Misc:        return "Misc";
    case Category::Internal:    return "Internal";
    }
    return "Misc";
}

/// @brief DrawComponentSection の Snapshot 版。unique_ptr を持つ型 (MeshCollider 等) 用。
/// @note 全体をコピーすると physics body が消えるので、CaptureFn / ApplyFn で保存対象の値だけを Undo へ持つ。
template<typename T, typename Snapshot, typename DrawFn, typename CaptureFn, typename ApplyFn>
void DrawComponentSectionCustom(
    scene::GameObject* go,
    EditorContext& ctx,
    std::any& compClipboard,
    const std::type_info*& compClipboardType,
    const char* label,
    DrawFn drawFn,
    CaptureFn captureFn,
    ApplyFn applyFn)
{
    auto* comp = go->GetComponent<T>();
    if (!comp) return;
    label = ComponentDisplayName<T>(label);

    if (ctx.inspectorComponentCollector && !ctx.inspectorComponentCollector->drawing) {
        const std::string key = label;
        ctx.editorSceneState.EnsureComponentOrder(go->instanceId, key);
        auto* collector = ctx.inspectorComponentCollector;
        collector->Add(key, [collector, go, &ctx, &compClipboard, &compClipboardType,
                             key, drawFn, captureFn, applyFn]() {
            collector->drawing = true;
            DrawComponentSectionCustom<T, Snapshot>(
                go, ctx, compClipboard, compClipboardType, key.c_str(),
                drawFn, captureFn, applyFn);
            collector->drawing = false;
        });
        return;
    }

    ImGui::PushID(label);
    const bool canUndo = CanRecordEditorUndo(ctx);

    auto pushUndoCmd = [&](const std::string& desc, const Snapshot& before, const Snapshot& after) {
        if (!ctx.undoStack || !ctx.activeScene) return;
        scene::Scene* sc = ctx.activeScene;
        const std::string iid = go->instanceId;
        const auto dirty = ctx.markSceneDirty;
        auto af = applyFn;
        auto doApply = [sc, iid, dirty, af](const Snapshot& v) {
            if (auto* target = sc->FindByGuid(iid))
                if (auto* c = target->GetComponent<T>()) {
                    af(*c, v);
                    if (dirty) dirty();
                }
        };
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            desc,
            [doApply, after]()  { doApply(after); },
            [doApply, before]() { doApply(before); }));
    };

    const ImU32 accent = ComponentAccent<T>();
    const Snapshot beforeEnabled = canUndo ? captureFn(*comp) : Snapshot{};
    const widgets::ComponentHeaderResult header = widgets::ComponentHeader(
        label, accent, &comp->enabled, true,
        MakeComponentReorderTarget(go, ctx, label));
    if (header.enabledChanged) {
        if (canUndo) pushUndoCmd(std::string("Toggle ") + label, beforeEnabled, captureFn(*comp));
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    const bool open = header.open;
    if (header.menuClicked)
        ImGui::OpenPopup("##comp_opts");

    bool removeRequested = false;
    if (ImGui::BeginPopup("##comp_opts")) {
        if (ImGui::MenuItem("Reset")) {
            const Snapshot before = captureFn(*comp);
            const bool wasEnabled = comp->enabled;
            /// @note Add Component と同じ既定値 (Renderer のメッシュから組み直す) へ戻す。
            applyFn(*comp, captureFn(MakeDefaultComponent<T>(*go)));
            comp->enabled = wasEnabled;
            if (canUndo) pushUndoCmd(std::string("Reset ") + label, before, captureFn(*comp));
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Component")) {
            compClipboard     = captureFn(*comp);
            compClipboardType = &typeid(T);
        }
        const bool canPaste = compClipboardType && *compClipboardType == typeid(T)
                           && compClipboard.type() == typeid(Snapshot);
        if (ImGui::MenuItem("Paste Component Values", nullptr, false, canPaste)) {
            const Snapshot before = captureFn(*comp);
            const bool wasEnabled = comp->enabled;
            applyFn(*comp, std::any_cast<Snapshot>(compClipboard));
            comp->enabled = wasEnabled;
            if (canUndo) pushUndoCmd(std::string("Paste ") + label, before, captureFn(*comp));
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        DrawPasteComponentAsNewMenuItem(*go, ctx, compClipboard, compClipboardType);
        ImGui::Separator();
        if (DrawRemoveComponentMenuItem(*go, typeid(T)))
            removeRequested = true;
        ImGui::EndPopup();
    }

    if (open) {
        static ComponentActiveEditCustom<T, Snapshot> edit{};

        const widgets::ComponentBodyScope body = widgets::BeginComponentBody(header, accent);
        ImGui::Spacing();
        if (!canUndo) {
            edit.active = false;
            drawFn(*comp, ctx);
        } else {
            const Snapshot beforeDraw = captureFn(*comp);
            const ImGuiID activeBefore = ImGui::GetActiveID();
            drawFn(*comp, ctx);
            const ImGuiID activeAfter = ImGui::GetActiveID();

            if (!edit.active && activeAfter != 0 && activeAfter != activeBefore) {
                edit.entityId = go->GetID();
                edit.activeId = activeAfter;
                edit.before   = beforeDraw;
                edit.active   = true;
            } else if (edit.active && edit.entityId != go->GetID()) {
                if (activeAfter != edit.activeId) edit.active = false;
            } else if (edit.active && activeAfter != edit.activeId) {
                const Snapshot after = captureFn(*comp);
                /// @note 値が動いていない操作 (参照欄のクリック等) では積まない。
                if (!ComponentSnapshotCompare<Snapshot>::Equal(edit.before, after)) {
                    pushUndoCmd(std::string("Change ") + label, edit.before, after);
                    if (ctx.markSceneDirty) ctx.markSceneDirty();
                }
                edit.active = false;
            }
        }
        ImGui::Spacing();
        widgets::EndComponentBody(body);
    }
    ImGui::Spacing();

    ImGui::PopID();

    if (!removeRequested) return;

    /// @note やり直しは空の T を足して Snapshot を当てる (collider は ApplyFn が捨て、物理側が組み直す)。
    const Snapshot removed = captureFn(*comp);
    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go->instanceId;
    const auto markDirty = ctx.markSceneDirty;
    auto af = applyFn;
    go->RemoveComponent<T>();
    if (canUndo && scene) {
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            std::string("Remove ") + label,
            [scene, instanceId, markDirty]() {
                if (auto* target = scene->FindByGuid(instanceId)) {
                    if (target->GetComponent<T>())
                        target->RemoveComponent<T>();
                    if (markDirty) markDirty();
                }
            },
            [scene, instanceId, removed, markDirty, af]() {
                if (auto* target = scene->FindByGuid(instanceId)) {
                    if (!target->GetComponent<T>()) {
                        auto& restored = target->AddComponent<T>();
                        af(restored, removed);
                    }
                    if (markDirty) markDirty();
                }
            }));
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

/// @note 明滅 (Flicker)。既定は Off なので、既存のライトでは «Flicker» の行 1 本しか増えない。
/// @note «Play 中だけ» と書き添えるのは、駆動する LightFlickerSystem は intensity という
/// @note 保存されるフィールドを書き換えるため、編集中は動かさないため。注記が無いと
/// @note «設定したのに Scene ビューが揺れない = 壊れている» と読める。
inline void DrawLightFlickerFields(scene::LightComponent& lc)
{
    using Flicker = scene::LightComponent::FlickerMode;

    ImGui::Separator();
    ImGui::SeparatorText("Flicker");

    static constexpr const char* kFlickerModeNames[] = { "Off", "Sine", "Noise", "Curve" };
    int flickerMode = static_cast<int>(lc.flickerMode);
    if (ImGui::Combo("Flicker Mode", &flickerMode, kFlickerModeNames,
                     IM_ARRAYSIZE(kFlickerModeNames)))
        lc.flickerMode = static_cast<Flicker>(flickerMode);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Off   : 無効 (既定)。intensity はそのまま\n"
            "Sine  : 滑らかな脈動。呼吸する光・ボスの目\n"
            "Noise : 不規則な揺らぎ。たいまつ・壊れた蛍光灯\n"
            "Curve : 下のカーブを 1 周期として繰り返す。放電のような作り込んだ形");
    }

    if (lc.flickerMode == Flicker::Off) {
        ImGui::TextDisabled("intensity は揺れません");
        return;
    }

    widgets::RangeField("Flicker Amplitude", lc.flickerAmplitude, 0.0f, 1.0f, "%.3f",
        "揺れの深さ。倍率 = 1 - Amplitude * (1 - 波形)。\n"
        "0 で無効 (常に 1 倍)、1 で消灯まで落ちます。\n"
        "オーサリングした Intensity は «ピーク» として扱われ、上へは振れません。");
    ImGui::DragFloat("Flicker Frequency", &lc.flickerFrequency, 0.05f, 0.0f, 60.0f, "%.2f Hz");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("1 秒あたりの周期数。Curve モードではカーブ 1 周の速さです。");
    if (lc.flickerFrequency < 0.0f) lc.flickerFrequency = 0.0f;

    if (lc.flickerMode != Flicker::Noise) {
        widgets::RangeField("Flicker Noise", lc.flickerNoise, 0.0f, 1.0f, "%.3f",
            "波形へ混ぜるノイズの量。0 で純粋な波形、1 で完全にノイズ。\n"
            "きれいな脈動に «不安定さ» を足すのに使います。");
    }

    widgets::RangeField("Flicker Phase", lc.flickerPhase, 0.0f, 1.0f, "%.3f",
        "位相オフセット。同じ設定のライトを並べたとき、揃って光らないようずらします。");

    int flickerSeed = static_cast<int>(lc.flickerSeed);
    if (ImGui::DragInt("Flicker Seed", &flickerSeed, 1.0f, 0, 65535)) {
        lc.flickerSeed = static_cast<std::uint32_t>(flickerSeed < 0 ? 0 : flickerSeed);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "ノイズ列の種。同じ Seed と同じ Phase なら、何度走らせても同じ揺れになります。\n"
            "隣のたいまつと «違う揺れ» にしたいときだけ変えてください。");
    }

    if (lc.flickerMode == Flicker::Curve) {
        /// @note 縦軸は倍率。1 を超えると増幅になるので、上限は 2 まで取る。
        widgets::CurveEditor("Flicker Curve", lc.flickerCurve, 2.0f);
    }

    ImGui::TextDisabled("揺れるのは Play 中だけです (保存値は Intensity のまま)");
}

/// @note dayNightDriven — SkyRenderer の昼夜カーブがこのライトの色/強度を上書きしている状態。
/// @note 上書きは RenderSystem が毎フレーム行うので、Inspector の値を編集しても画面は変わらない。
/// @note 引数で受けて出典を示さないと «ライトが壊れている» と読めてしまう。
inline void DrawLightFields(scene::GameObject& go, scene::LightComponent& lc,
                            bool dayNightDriven = false,
                            const std::string& projectRoot = {})
{
    static constexpr const char* kTypeNames[] = {
        "Directional", "Point", "Spot", "Area (Rect)", "Sphere", "Tube" };
    int typeIdx = static_cast<int>(lc.type);
    if (ImGui::Combo("Type", &typeIdx, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
        lc.type = static_cast<scene::LightComponent::Type>(typeIdx);

    const bool drivenBySky = dayNightDriven && lc.type == scene::LightComponent::Type::Directional;
    ImGui::BeginDisabled(drivenBySky);
    ImGui::Checkbox("Use Color Temperature", &lc.useColorTemperature);
    if (lc.useColorTemperature) {
        ImGui::DragFloat("Temperature", &lc.colorTemperature, 25.0f, 1000.0f, 15000.0f, "%.0f K");
        /// @note 温度から作った色をそのまま見せる。数値だけでは何色になるか分からない。
        const math::Vector3 preview = renderer::ColorFromTemperature(lc.colorTemperature);
        ImGui::ColorButton("##tempPreview",
                           ImVec4(preview.x, preview.y, preview.z, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(0.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("1900=ろうそく 2700=白熱灯 4000=蛍光灯 6500=昼光 10000=日陰");
    } else {
        widgets::ColorEdit3("Color", lc.color);
    }
    /// @note Area は上限 200 では足りない。単位が「面の輝度」で、小さなパネルほど
    /// @note 大きな値が要るため (LightComponent.hpp の intensity の説明を参照)。
    const bool isArea = (lc.type == scene::LightComponent::Type::Area);
    ImGui::DragFloat("Intensity", &lc.intensity, isArea ? 1.0f : 0.05f,
                     0.0f, isArea ? 2000.0f : 200.0f);
    /// @note タイプごとに単位が違うことを、値を触る場所で明示する。Point の感覚のまま Area へ
    /// @note 10 を入れると albedo x 0.04 でほぼ見えず、«実装が壊れている» と読み違える (実例あり)。
    ImGui::SameLine();
    switch (lc.type) {
    case scene::LightComponent::Type::Directional:
        ImGui::TextDisabled("放射照度 (1.0 = albedo そのまま)"); break;
    case scene::LightComponent::Type::Area:
        ImGui::TextColored({ 1.0f, 0.80f, 0.40f, 1.0f },
                           "面の輝度。1/d^2 は掛からない — 天井照明なら 100〜200");
        break;
    default:
        ImGui::TextDisabled("1m 地点の明るさ (1/d^2 減衰)。明るい屋外なら 15〜30");
        break;
    }
    ImGui::EndDisabled();
    if (drivenBySky) {
        ImGui::TextColored({ 1.0f, 0.75f, 0.35f, 1.0f },
                           "Sky Renderer の Day Night が上書き中 (向きはこのライトのまま)");
    }

    if (lc.type != scene::LightComponent::Type::Directional) {
        float pos[3] = {
            go.transform.position.x,
            go.transform.position.y,
            go.transform.position.z
        };
        if (widgets::DragAxes("Position", pos, 3, 0.1f))
            go.transform.position = { pos[0], pos[1], pos[2] };
        ImGui::DragFloat("Range", &lc.range, 0.1f, 0.0f, 500.0f);
    }

    if (lc.type == scene::LightComponent::Type::Spot) {
        ImGui::DragFloat("Inner Cone", &lc.innerCone, 0.5f, 0.0f, 89.0f);
        ImGui::DragFloat("Outer Cone", &lc.outerCone, 0.5f, 0.0f, 89.0f);
    }

    if (lc.type == scene::LightComponent::Type::Area) {
        ImGui::DragFloat("Width",  &lc.areaWidth,  0.05f, 0.01f, 100.0f, "%.2f m");
        ImGui::DragFloat("Height", &lc.areaHeight, 0.05f, 0.01f, 100.0f, "%.2f m");
        ImGui::Checkbox("Two Sided", &lc.areaTwoSided);
        ImGui::TextDisabled("面の向きは Transform の Forward。板の裏は Two Sided で照らす");
    }

    /// @note 発光体の大きさ。Area だけは幅と高さがその役割なので出さない。
    if (lc.type != scene::LightComponent::Type::Directional &&
        lc.type != scene::LightComponent::Type::Area) {
        ImGui::DragFloat("Source Radius", &lc.sourceRadius, 0.005f, 0.0f, 10.0f, "%.3f m");
        if (lc.type == scene::LightComponent::Type::Tube) {
            ImGui::DragFloat("Source Length", &lc.sourceLength, 0.05f, 0.0f, 50.0f, "%.2f m");
            ImGui::TextDisabled("管の軸は Transform の Right");
        } else if (lc.sourceRadius <= 0.0f) {
            ImGui::TextDisabled("0 = 厳密な点光源。上げるとハイライトと影の縁が柔らかくなる");
        }
    }

    if (lc.type != scene::LightComponent::Type::Point &&
        lc.type != scene::LightComponent::Type::Sphere) {
        auto fwd = go.transform.forward;
        float dir[3] = { fwd.x, fwd.y, fwd.z };
        ImGui::InputFloat3("Forward", dir, "%.3f", ImGuiInputTextFlags_ReadOnly);
    }

    ImGui::Separator();
    ImGui::SeparatorText("Shadow");
    ImGui::Checkbox("Cast Shadows", &lc.castShadows);
    if (lc.castShadows) {
        ImGui::DragFloat("Shadow Strength", &lc.shadowStrength, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Shadow Bias",     &lc.shadowBias,     0.05f, 0.1f, 10.0f);
        if (lc.type == scene::LightComponent::Type::Directional) {
            /// @note 0 のとき "Auto" 表示。シーン全体の AABB から自動フィット。
            const char* distFmt = (lc.shadowDistance <= 0.0f) ? "Auto" : "%.1f m";
            ImGui::DragFloat("Shadow Distance", &lc.shadowDistance, 5.0f, 0.0f, 2000.0f, distFmt);
        } else {
            ImGui::DragFloat("Shadow Near Plane", &lc.shadowNearPlane,
                             0.01f, 0.01f, 10.0f, "%.2f m");
            /// @note タイルの取り合いは «置いたのに影が出ない» の唯一の原因なので、
            /// @note この型が何枚使うかをその場で出す。
            const bool cube = lc.type == scene::LightComponent::Type::Point
                           || lc.type == scene::LightComponent::Type::Sphere
                           || lc.type == scene::LightComponent::Type::Tube;
            ImGui::TextDisabled(cube
                ? "アトラスのタイルは 16 枚。この型は全方位なのでキューブ 6 面 = 6 枚"
                : "アトラスのタイルは 16 枚。この型は 1 枚");
            ImGui::TextDisabled("割り当てはカメラに近い順。あふれた光源は影を落とさない");
            if (lc.type == scene::LightComponent::Type::Area)
                ImGui::TextDisabled("Area は法線方向 75 度ぶんだけ。真横へは影が出ない");
            if (lc.type == scene::LightComponent::Type::Tube)
                ImGui::TextDisabled("管の長さは影の形に効かない (中心から焼く)。ぼけ幅は Source Radius");
        }
    }

    if (lc.type == scene::LightComponent::Type::Spot) {
        ImGui::Separator();
        ImGui::SeparatorText("Cookie");
        widgets::AssetPathField("Cookie", lc.cookiePath, ".png,.jpg,.dds,.tga", projectRoot);
        if (!lc.cookiePath.empty())
            ImGui::DragFloat("Cookie Rotation", &lc.cookieRotation, 1.0f, -180.0f, 180.0f, "%.0f deg");
    }

    DrawLightFlickerFields(lc);
}
/// @note SyncColliderPreview — Inspector で形状を編集した直後に physics::Collider へ反映する。
/// @note 姿勢の反映は Engine 側の ColliderSync に一本化する (独自コピーだと world とローカルを
/// @note 取り違えて、親を持つオブジェクトでプレビューがずれる)。
template<typename T>
void SyncColliderPreview(scene::GameObject& go, T& col)
{
    scene::SyncColliderShape(col, go.transform.worldScale);
    if (!col.collider) return;

    bool useTransformScale = true;
    if constexpr (std::is_same_v<T, scene::MeshColliderComponent> ||
                  std::is_same_v<T, scene::ConvexHullColliderComponent>) {
        useTransformScale = col.useTransformScale;
    }
    scene::UpdateColliderPose(go, col, useTransformScale);
}
inline std::string SanitizeTerrainAssetName(const std::string& name)
{
    std::string result = name.empty() ? "Terrain" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}
inline std::string UniqueTerrainAssetPath(const EditorContext& ctx, const std::string& objectName)
{
    const std::string assetRoot = ctx.projectRoot.empty()
        ? "Assets"
        : ctx.projectRoot + "/Assets";
    const std::string terrainDir = assetRoot + "/Terrain";
    util::FileSystem::EnsureDirectory(terrainDir);

    const std::string base = terrainDir + "/" + SanitizeTerrainAssetName(objectName);
    std::string path = base + ".terrain";
    for (int i = 1; util::FileSystem::Exists(path) && i < 10000; ++i)
        path = base + " " + std::to_string(i) + ".terrain";
    return NormalizeAssetPath(path);
}
inline std::string TerrainAssetDiskPath(const EditorContext& ctx, const std::string& assetPath)
{
    return ToProjectAssetDiskPath(ctx.projectRoot, assetPath);
}

/// @brief .mat の assets/ 相対パスを保存 API に渡せるディスクパスへ変換する。
/// @note MaterialComponent はポータブルな Assets 起点パスだけを保持するため、Editor の保存時だけ projectRoot を補完する。
inline std::string MaterialAssetDiskPath(const EditorContext& ctx, const std::string& assetPath)
{
    return ToProjectAssetDiskPath(ctx.projectRoot, assetPath);
}

/// @note ある GameObject が現在持っている «登録済みコンポーネント型» の一覧を返す。
/// @note Add Component の Undo を作るための基準点。追加前後でこの集合を比べれば、
/// @note 依存で一緒に付いたコンポーネントも含めて «増えたぶん» だけが取り出せる。
inline std::vector<std::type_index> CapturePresentComponentTypes(scene::GameObject& go)
{
    std::vector<std::type_index> types;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (go.GetComponent<T>()) types.emplace_back(typeid(T));
    });
    return types;
}

/// @note before に無かった型だけを対象に、Redo 用の「再追加関数」と Undo 用の「削除関数」を集める。
inline void CollectAddedComponentOps(
    scene::GameObject& go,
    const std::vector<std::type_index>& before,
    std::vector<std::function<void(scene::GameObject&)>>& outAdders,
    std::vector<std::function<void(scene::GameObject&)>>& outRemovers)
{
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        auto* comp = go.GetComponent<T>();
        if (!comp) return;
        if (std::find(before.begin(), before.end(), std::type_index(typeid(T))) != before.end())
            return;

        /// @note Redo は「追加直後の値」をそのまま復元する。
        /// @note unique_ptr を含むなどコピーできない型 (MeshCollider 等) は値を持ち運べないため、
        /// @note 既定の追加経路をもう一度走らせる — 追加直後と同じ結果になる。
        if constexpr (std::is_copy_constructible_v<T>) {
            T value = *comp;
            outAdders.push_back([value](scene::GameObject& target) {
                if (!target.GetComponent<T>()) target.AddComponent<T>(value);
            });
        } else {
            outAdders.push_back([](scene::GameObject& target) {
                if (!target.GetComponent<T>()) AddRegisteredComponent<T>(target);
            });
        }
        outRemovers.push_back([](scene::GameObject& target) {
            if (target.GetComponent<T>()) target.RemoveComponent<T>();
        });
    });
}

/// @brief before から増えた登録型だけを足し引きする Undo コマンドを作る。
/// @return 記録不可、または何も増えていなければ nullptr。
/// @note シーン全体のスナップショットにすると EntityID が振り直され、選択やロックが消える。
/// @note GameObject* は Undo までに無効化され得るので instanceId から引き直す。
inline std::unique_ptr<ICommand> MakeAddedComponentsCommand(
    scene::GameObject& go,
    EditorContext& ctx,
    const std::vector<std::type_index>& before,
    std::string description)
{
    if (!(CanRecordEditorUndo(ctx) && ctx.activeScene)) return nullptr;

    std::vector<std::function<void(scene::GameObject&)>> adders;
    std::vector<std::function<void(scene::GameObject&)>> removers;
    CollectAddedComponentOps(go, before, adders, removers);
    if (adders.empty()) return nullptr;

    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;
    auto apply = [scene, instanceId, markDirty](
                     const std::vector<std::function<void(scene::GameObject&)>>& ops) {
        auto* target = scene->FindByGuid(instanceId);
        if (!target) return;
        for (const auto& op : ops) op(*target);
        if (markDirty) markDirty();
    };

    return std::make_unique<LambdaCommand>(
        std::move(description),
        [apply, adders]()   { apply(adders); },
        [apply, removers]() { apply(removers); });
}

/// @brief 登録済みコンポーネントを既定値と依存込みで追加し、増えた型だけを戻す Undo を返す。
template<typename T>
std::unique_ptr<ICommand> AddRegisteredComponentWithUndo(scene::GameObject& go,
                                                         EditorContext& ctx,
                                                         const char* label)
{
    const bool canRecordUndo = CanRecordEditorUndo(ctx) && ctx.activeScene;
    std::vector<std::type_index> before;
    if (canRecordUndo) before = CapturePresentComponentTypes(go);

    AddRegisteredComponent<T>(go);
    if (!canRecordUndo) return nullptr;
    return MakeAddedComponentsCommand(go, ctx, before, std::string("Add ") + label);
}

/// @brief 型名で «そのコンポーネントを持っているか» を答える。
/// @note 内部型 (addable = false) も対象。Bone のようにエンジンが張るものを FBZZ_REF で指す場面がある。
inline bool HasRegisteredComponentByName(scene::GameObject& go, std::string_view typeName)
{
    bool found = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (found) return;
        if (typeName != Registration::serializedName) return;
        found = go.GetComponent<T>() != nullptr;
    });
    return found;
}

/// @brief 不足している必須コンポーネントをまとめて追加する。Inspector の "Fix" ボタンの実体。
/// @return 1 操作 = 1 コマンドの Undo。何も増えなければ nullptr。
inline std::unique_ptr<ICommand> AddMissingComponentsWithUndo(
    scene::GameObject& go,
    EditorContext& ctx,
    const std::vector<std::string>& typeNames)
{
    const bool canRecordUndo = CanRecordEditorUndo(ctx) && ctx.activeScene;
    std::vector<std::type_index> before;
    if (canRecordUndo) before = CapturePresentComponentTypes(go);

    bool anyAdded = false;
    for (const std::string& typeName : typeNames)
        anyAdded |= AddRegisteredComponentByName(go, typeName);

    if (!anyAdded || !canRecordUndo) return nullptr;
    return MakeAddedComponentsCommand(go, ctx, before, "Add Missing Components");
}

/// @brief go が typeName のスクリプトを既に持っているか。
/// @note 未ロードで型名だけ保持しているエントリ (serialized) も «持っている» に数える。
inline bool HasScriptOfType(scene::GameObject& go, std::string_view typeName)
{
    const auto* sc = go.GetComponent<scene::ScriptComponent>();
    if (!sc) return false;
    for (const scene::ScriptEntry& entry : sc->scripts) {
        if (entry.script && typeName == entry.script->GetTypeName()) return true;
        if (!entry.script && entry.serialized && typeName == entry.serialized->type) return true;
    }
    return false;
}

/// @brief スクリプトを 1 件と、その FBZZ_REQUIRE_COMPONENT の不足分を追加する。
/// @return 追加したスクリプトと、この操作で増えたコンポーネントだけを戻す 1 コマンド。
/// @note スクリプト追加は ScriptComponent::scripts への要素追加なので、型の増減差分だけでは戻せない。
/// @note 要求コンポーネントはスクリプトより先に付ける (ScriptObjectFactory と同じ順)。
inline std::unique_ptr<ICommand> AddScriptWithUndo(scene::GameObject& go,
                                                   EditorContext& ctx,
                                                   const std::string& typeName)
{
    auto script = scene::ScriptFactory::Create(typeName);
    if (!script) return nullptr;

    const bool canRecordUndo = CanRecordEditorUndo(ctx) && ctx.activeScene;
    std::vector<std::type_index> before;
    if (canRecordUndo) before = CapturePresentComponentTypes(go);
    for (const std::string& required : script->RequiredComponents())
        AddRegisteredComponentByName(go, required);
    std::unique_ptr<ICommand> requiredCommand =
        canRecordUndo ? MakeAddedComponentsCommand(go, ctx, before, "Add Required Components")
                      : nullptr;
    std::shared_ptr<ICommand> required = std::move(requiredCommand);

    script->SetContext(ctx.activeScene, &go);
    script->Reset();
                script->ExecuteProfiledCallback(&scene::Script::OnValidate, scene::ScriptCallbackKind::VALIDATE, "OnValidate");

    const bool hadComponent = go.GetComponent<scene::ScriptComponent>() != nullptr;
    auto* sc = go.GetComponent<scene::ScriptComponent>();
    if (!sc) sc = &go.AddComponent<scene::ScriptComponent>();

    scene::ScriptEntry& entry = sc->scripts.emplace_back();
    entry.script = std::move(script);
    const std::size_t addedIndex = sc->scripts.size() - 1;

    if (!canRecordUndo) return nullptr;

    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;

    return std::make_unique<LambdaCommand>(
        std::string("Add Script ") + typeName,
        [scene, instanceId, typeName, required, markDirty]() {
            if (required) required->Execute();
            auto* target = scene->FindByGuid(instanceId);
            if (!target) return;
            auto newScript = scene::ScriptFactory::Create(typeName);
            if (!newScript) return;
            auto* comp = target->GetComponent<scene::ScriptComponent>();
            if (!comp) comp = &target->AddComponent<scene::ScriptComponent>();
            newScript->SetContext(scene, target);
            newScript->Reset();
                    newScript->ExecuteProfiledCallback(&scene::Script::OnValidate, scene::ScriptCallbackKind::VALIDATE, "OnValidate");
            comp->scripts.emplace_back().script = std::move(newScript);
            if (markDirty) markDirty();
        },
        [scene, instanceId, addedIndex, hadComponent, required, markDirty]() {
            auto* target = scene->FindByGuid(instanceId);
            if (!target) return;
            if (auto* comp = target->GetComponent<scene::ScriptComponent>()) {
                if (addedIndex < comp->scripts.size())
                    comp->scripts.erase(comp->scripts.begin()
                                        + static_cast<std::ptrdiff_t>(addedIndex));
                if (!hadComponent && comp->scripts.empty())
                    target->RemoveComponent<scene::ScriptComponent>();
            }
            if (required) required->Undo();
            if (markDirty) markDirty();
        });
}

/// @brief クリップボードの値を «まだ持っていない型» として go へ追加できるか。
/// @note Snapshot 型で保持したクリップボード (MeshCollider 等) と内部型は対象外。
inline bool CanPasteComponentAsNew(scene::GameObject& go,
                                   const std::any& clipboard,
                                   const std::type_info* clipboardType)
{
    if (clipboardType == nullptr || clipboard.type() != *clipboardType) return false;
    bool can = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (Registration::addable && std::is_copy_constructible_v<T>) {
            if (*clipboardType == typeid(T)) can = go.GetComponent<T>() == nullptr;
        }
    });
    return can;
}

/// @brief クリップボードの値で型を新規追加し、依存も足す。
/// @return 増えた型だけを戻す Undo。追加できなければ nullptr で go は未変更。
inline std::unique_ptr<ICommand> PasteComponentAsNewWithUndo(scene::GameObject& go,
                                                             EditorContext& ctx,
                                                             const std::any& clipboard,
                                                             const std::type_info* clipboardType)
{
    if (!CanPasteComponentAsNew(go, clipboard, clipboardType)) return nullptr;

    const bool canRecordUndo = CanRecordEditorUndo(ctx) && ctx.activeScene;
    std::vector<std::type_index> before;
    if (canRecordUndo) before = CapturePresentComponentTypes(go);

    std::string description = "Paste Component As New";
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (Registration::addable && std::is_copy_constructible_v<T>) {
            if (*clipboardType != typeid(T)) return;
            T value = std::any_cast<const T&>(clipboard);
            /// @note コピー元の GPU ハンドルを共有すると片方の破棄でもう片方が消えた枠を掴む。
            scene::ClearComponentGpuHandles(value);
            go.AddComponent<T>(std::move(value));
            AddComponentDependencies<T>(go);
            description = std::string("Paste ") + Registration::displayName + " As New";
        }
    });

    if (!canRecordUndo) return nullptr;
    return MakeAddedComponentsCommand(go, ctx, before, std::move(description));
}

/// @brief "Paste Component As New" のメニュー項目。カードのメニューと Inspector の右クリックが共有する。
inline void DrawPasteComponentAsNewMenuItem(scene::GameObject& go,
                                            EditorContext& ctx,
                                            const std::any& clipboard,
                                            const std::type_info* clipboardType)
{
    const bool canPaste = CanPasteComponentAsNew(go, clipboard, clipboardType);
    if (!ImGui::MenuItem("Paste Component As New", nullptr, false, canPaste)) return;
    if (auto command = PasteComponentAsNewWithUndo(go, ctx, clipboard, clipboardType))
        ctx.undoStack->Push(std::move(command));
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

inline bool ComponentMatchesFilter(const char* label, const char* filter)
{
    if (filter[0] == '\0') return true;
    return util::StringUtils::ContainsCI(label, filter);
}

template<typename DrawItems>
inline bool AddComponentCategory(const char* label, const char* filter, DrawItems drawItems)
{
    if (filter[0] == '\0') {
        if (ImGui::BeginMenu(label)) {
            drawItems(label, "");
            ImGui::EndMenu();
        }
        return true;
    }

    return drawItems(label, filter);
}
/// @brief Add Component メニュー。targets 全体へまとめて追加し、1 回の Undo で戻る。
/// @note 単体は «要素 1 個のリスト» として同じ経路に乗る。
inline void DrawAddComponentMenuMulti(const std::vector<scene::GameObject*>& targets,
                                      char (&filterBuffer)[64],
                                      EditorContext& ctx,
                                      const char* buttonLabel)
{
    if (targets.empty()) return;
    /// @note 最も押されるボタンなので、カード列の底に埋もれないようアクセント色 + 1 段高い枠にする。
    ImGui::PushStyleColor(ImGuiCol_Button,        EditorTheme::Color(ThemeColor::AccentSoft));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(ThemeColor::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  EditorTheme::Color(ThemeColor::AccentActive));
    const bool addClicked =
        ImGui::Button(buttonLabel, { -1.0f, ImGui::GetFrameHeight() + 6.0f });
    ImGui::PopStyleColor(3);
    if (addClicked)
        ImGui::OpenPopup("##add_component");

    if (!ImGui::BeginPopup("##add_component")) return;

    if (ImGui::IsWindowAppearing()) {
        filterBuffer[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##comp_search", "Search...", filterBuffer, sizeof(filterBuffer));
    ImGui::Separator();

    const char* filter  = filterBuffer;
    bool        anyShown = false;
    bool        didAdd = false;

    /// @note 対象ごとの Undo を 1 コマンドへ束ねる (20 個へ足した操作を Ctrl+Z 20 回にしない)。
    auto AddToAllTargets = [&targets](auto addOne, const std::string& description)
        -> std::unique_ptr<ICommand> {
        auto composite = std::make_unique<CompositeCommand>(description);
        for (auto* target : targets) {
            if (!target) continue;
            if (auto cmd = addOne(*target)) composite->Add(std::move(cmd));
        }
        if (composite->Empty()) return nullptr;
        return composite;
    };

    /// @note perform() は追加を実行して戻す Undo (不要なら nullptr) を返す。差分の作り方は呼び出し側が知っている。
    auto addItem = [&](const char* category, const char* label, bool enabled, auto perform) -> bool {
        char path[128];
        std::snprintf(path, sizeof(path), "%s/%s", category, label);
        char colonPath[128];
        std::snprintf(colonPath, sizeof(colonPath), "%s: %s", category, label);
        if (!ComponentMatchesFilter(path, filter) &&
            !ComponentMatchesFilter(colonPath, filter) &&
            !ComponentMatchesFilter(label, filter))
            return false;
        if (ImGui::MenuItem(filter[0] == '\0' ? label : path, nullptr, false, enabled)) {
            std::unique_ptr<ICommand> command = perform();
            if (command && CanRecordEditorUndo(ctx))
                ctx.undoStack->Push(std::move(command));
            if (ctx.markSceneDirty) ctx.markSceneDirty();
            didAdd = true;
            ImGui::CloseCurrentPopup();
        }
        return true;
    };

    static constexpr scene::ComponentCategory kCategories[] = {
        scene::ComponentCategory::Rendering,
        scene::ComponentCategory::Lighting,
        scene::ComponentCategory::Physics,
        scene::ComponentCategory::Animation,
        scene::ComponentCategory::Audio,
        scene::ComponentCategory::Effects,
        scene::ComponentCategory::Environment,
        scene::ComponentCategory::Navigation,
        scene::ComponentCategory::Terrain,
        scene::ComponentCategory::UI,
        scene::ComponentCategory::Misc
    };

    for (const scene::ComponentCategory selectedCategory : kCategories) {
        bool hasRegisteredItems = false;
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (Registration::addable)
                hasRegisteredItems |= Registration::category == selectedCategory;
        });
        if (!hasRegisteredItems) continue;

        const char* categoryLabel = ComponentCategoryLabel(selectedCategory);
        anyShown |= AddComponentCategory(categoryLabel, filter, [&](const char* category, const char*) {
            bool shown = false;
            scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
                if constexpr (Registration::addable) {
                    if (Registration::category != selectedCategory) return;
                    /// @note 1 体でも未所持なら追加できる (所持済みの対象は飛ばす)。
                    bool anyMissing = false;
                    for (auto* target : targets)
                        if (!target->GetComponent<T>()) { anyMissing = true; break; }

                    shown |= addItem(category, Registration::displayName, anyMissing, [&]() {
                        return AddToAllTargets([&](scene::GameObject& target) {
                            return target.GetComponent<T>()
                                ? nullptr
                                : AddRegisteredComponentWithUndo<T>(target, ctx,
                                                                    Registration::displayName);
                        }, std::string("Add ") + Registration::displayName);
                    });
                }
            });
            return shown;
        });
    }

    const auto scriptTypeNames = scene::ScriptFactory::RegisteredTypeNames();
    if (!scriptTypeNames.empty()) {
        anyShown |= AddComponentCategory("Scripts", filter, [&](const char* category, const char*) {
            bool shown = false;
            for (const std::string& typeName : scriptTypeNames) {
                bool anyMissing = false;
                for (auto* target : targets)
                    if (!HasScriptOfType(*target, typeName)) { anyMissing = true; break; }

                shown |= addItem(category, typeName.c_str(), anyMissing, [&]() {
                    return AddToAllTargets([&](scene::GameObject& target) {
                        return HasScriptOfType(target, typeName)
                            ? nullptr
                            : AddScriptWithUndo(target, ctx, typeName);
                    }, "Add Script " + typeName);
                });
            }
            return shown;
        });
    }

    if (!anyShown)
        ImGui::TextDisabled("No results");

    ImGui::EndPopup();

    if (didAdd)
        filterBuffer[0] = '\0';
}

/// @note 単一 GameObject 版 (要素 1 個のリストとして同じ経路へ乗せる)。
inline void DrawAddComponentMenu(scene::GameObject& go, char (&filterBuffer)[64], EditorContext& ctx)
{
    DrawAddComponentMenuMulti({ &go }, filterBuffer, ctx, "Add Component");
}

/// @note widgets::DragVec3 の 2 成分版。ラベル列・軸色を Vector3 の行と揃えるため同じ構成で描く。
inline bool DragVec2(const char* label, math::Vector2& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f)
{
    const widgets::PropertyRowScope row = widgets::BeginPropertyField(label);
    float data[2] = { value.x, value.y };
    const bool changed = widgets::DragAxes("##v", data, 2, speed, min, max);
    if (changed) value = { data[0], data[1] };
    widgets::EndPropertyField(row);
    return changed;
}



void DrawTransformInspectors(scene::GameObject* go, EditorContext& ctx);
void DrawRenderingInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawAnimationInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawMaterialInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawLightingInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawEffectsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawPhysicsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawEnvironmentInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawAutomaticInspectors(scene::ComponentCategory category,
                             scene::GameObject* go,
                             EditorContext& ctx,
                             std::any& componentClipboard,
                             const std::type_info*& componentClipboardType);
void DrawTerrainWaterInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
void DrawNavigationInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType);
/// @note スクリプトカードを Component 並びへ個別登録するための描画ライフサイクル。
/// @note Begin/End は 1 GameObject の全カードを囲み、DrawScriptCard は 1 枚だけ描く。
std::string GetScriptOrderKey(const scene::ScriptComponent& component, int index);
void BeginScriptInspectorFrame(scene::GameObject* go, EditorContext& ctx);
void DrawScriptCard(scene::GameObject* go, EditorContext& ctx, int index);
void EndScriptInspectorFrame(scene::GameObject* go, EditorContext& ctx);
void DrawScriptInspectors(scene::GameObject* go, EditorContext& ctx);

} /// @note namespace fbzz::editor
