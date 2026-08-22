// FBZZ Engine
// InspectorCommon.hpp | fbzz::editor
// Inspector のカテゴリ分割ファイルで共有する描画ヘルパー
#pragma once

#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ColliderFit.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
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
#include <Engine/Scene/Components/ParticleForceField.hpp>
#include <Engine/Scene/Components/WindZoneComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <cstring>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/AtmosphericScatteringComponent.hpp>
#include <Engine/Scene/Components/PostProcessVolumeComponent.hpp>
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
// ComponentUndoCompare の特化で参照する。
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/Components/FoliageComponent.hpp>
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

// ── Undo 判定 ────────────────────────────────────────────────────────────────
// 「ユーザーが Inspector で編集しうる値」が変わったかを比較する。
//
// 従来は「ImGui の ActiveID が動いた = 編集した」とみなして積んでいた。しかし
// ActiveID はクリックや折りたたみの開閉でも動くため、参照先を見に行っただけで
// 中身の変わらない履歴が残っていた。この判定があると、編集ウィジェットを
// コンポーネントのセクション内へ自由に置けるようになる。
//
// WHY 構造体全体を比較しないか (重要):
//   コンポーネントはランタイム状態を同じ構造体に持っている — GPU ハンドル、ボーンの
//   EntityID、スキニング済み頂点バッファ、ベイク結果、dirty フラグ、LOD の可視フラグ。
//   これらはシステムが毎フレーム書き換えるため、全体比較にすると「触っていないのに
//   毎フレーム差分あり」になって Undo が溢れる。
//
// WHY Reflect() を基準にするか:
//   Reflect() に載っていないフィールドは SceneSerializer にも保存されない。つまり
//   「Reflect() されている = 永続的なユーザー状態」がほぼ成り立つ。1 つ 1 つ手で
//   フィールドを列挙するより、リフレクションを通した値を比較する方が短く、
//   フィールドを増やしたときに比較を書き忘れて Undo が静かに壊れることもない。
//
//   例外は「Reflect() では表現できず SceneSerializer が専用コードで読み書きする」
//   フィールドを持つコンポーネント (vector<struct> 等)。そちらは下で個別に特化し、
//   リフレクションの digest に加えてその配列も比較する。

// Reflect() された値をすべて 1 本の文字列へ落とすリフレクタ。
// WHY 文字列へ落とすか: 型ごとの比較関数を書かずに済み、フィールドの追加・削除にも
//     自動で追従する。呼ばれるのは 1 操作の終わりだけなので毎フレームのコストにならない。
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

    // 既定実装が空の入口も埋める。埋め忘れるとその型のフィールドが digest に載らず、
    // 「編集したのに Undo できない」側へ倒れる。
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

// component の Reflect() を通した値の digest。
// WHY 非 const 参照を取るか: Reflect() は値を書き戻す実装 (enum のクランプ等) があり
//     const では呼べない。呼び出し側はどちらも実体を持っているので問題にならない。
template<typename T>
[[nodiscard]] std::string CaptureComponentDigest(T& component)
{
    ComponentReflectDigest digest;
    component.Reflect(digest);
    return digest.Result();
}

// Reflect() がそのコンポーネントの編集可能な状態を完全に覆っていることの宣言。
// 既定は false = 従来どおり必ず Undo を積む (安全側)。
//
// WHY オプトインにするか: 宣言を書き忘れても挙動は今までと同じ (履歴が少し多い) で済む。
//     逆に既定を true にすると、Reflect() が不完全なコンポーネントで
//     「編集したのに Undo できない」という取り返しのつかない壊れ方になる。
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

// Reflect() が編集可能な状態を完全に覆っているコンポーネント。
// (Reflect() に載っていないフィールドは SceneSerializer にも保存されないため、
//  この一覧は「専用の保存コードを持たないコンポーネント」と一致する)
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
FBZZ_COMPONENT_UNDO_REFLECTS(LightComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(MeshColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(MeshTrailComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshAgentComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshModifierComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshOffMeshLinkComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshSensorComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(NavMeshSurfaceComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(ParticleEmitter)
FBZZ_COMPONENT_UNDO_REFLECTS(ParticleForceField)
FBZZ_COMPONENT_UNDO_REFLECTS(PostProcessVolumeComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(ReflectionProbeComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(RigidBodyComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(SkyRenderer)
FBZZ_COMPONENT_UNDO_REFLECTS(SphereColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(SunMoonRenderer)
FBZZ_COMPONENT_UNDO_REFLECTS(TerrainColliderComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(TrailComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(VFXGraphComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(VolumeComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(VolumetricCloudComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(WaterComponent)
FBZZ_COMPONENT_UNDO_REFLECTS(WindZoneComponent)

#undef FBZZ_COMPONENT_UNDO_REFLECTS

// ── Reflect() だけでは足りないコンポーネントの特化 ────────────────────────────
// いずれも「digest + Reflect に載らない編集対象」を比較する。
// digest 側でカバーされるフィールドは重複して書かない (増減に自動追従させる)。

// 固定長配列・vector<struct> の比較ヘルパー。
template<typename T, typename Equal>
[[nodiscard]] bool RangesEqual(const std::vector<T>& a, const std::vector<T>& b, Equal equal)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!equal(a[i], b[i])) return false;
    return true;
}

// DrawComponentSectionCustom (unique_ptr を持つため専用 Snapshot を使うコンポーネント) の
// 変更判定。Snapshot が operator== を持っていればそれを使い、無ければ従来どおり必ず積む。
//
// WHY 型特化ではなく operator== の検出にするか:
//   Snapshot 型は使う側の .cpp の無名名前空間で定義されている。ヘッダー側から
//   明示的特化を書けないため、Snapshot に == を足すだけで有効になる形にしておく。
template<typename Snapshot>
struct ComponentSnapshotCompare {
    static bool Equal(const Snapshot& a, const Snapshot& b)
    {
        if constexpr (requires { a == b; }) return a == b;
        else return false;
    }
};

// マテリアルスロット 1 つぶんの、ユーザーが編集しうる値。
// paramOverrides / textureOverrides / 各 override フラグはスクリプトが実行中に
// 書き込むランタイム専用の状態なので比較に含めない。
inline bool MaterialSlotUserValuesEqual(const scene::MaterialSlot& a,
                                        const scene::MaterialSlot& b)
{
    return a.materialPath == b.materialPath && a.visible == b.visible;
}

// Renderer 系は mesh / model が生ポインタで Reflect に載らない (パスから解決される)。
// Inspector でメッシュを差し替えるとポインタだけが変わる瞬間があるため明示的に見る。
// lodVisible は LODSystem が毎フレーム書き換えるランタイム値なので除く。
template<>
struct ComponentUndoCompare<scene::MeshRenderer> {
    static bool UserValuesEqual(scene::MeshRenderer& a, scene::MeshRenderer& b)
    {
        return a.mesh == b.mesh
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

// nodeEntities / morphWeights / 各頂点バッファ / gpuSkinnedThisFrame は
// AnimatorSystem と SkinningComputePass が毎フレーム書き換えるため除く。
// submeshIndices は配置時に決まり Reflect には載らないので明示的に見る。
template<>
struct ComponentUndoCompare<scene::SkinnedMeshRenderer> {
    static bool UserValuesEqual(scene::SkinnedMeshRenderer& a, scene::SkinnedMeshRenderer& b)
    {
        return a.model          == b.model
            && a.submeshIndices == b.submeshIndices
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

// MaterialComponent::Reflect はスロット 0 の materialPath までしか載せない。
// extraSlots (submesh 1 以降) は SceneSerializer が専用コードで読み書きする。
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

// layerCullDistances は固定長配列で IReflector に対応する Field が無い。
template<>
struct ComponentUndoCompare<scene::CameraComponent> {
    static bool UserValuesEqual(scene::CameraComponent& a, scene::CameraComponent& b)
    {
        for (size_t i = 0; i < std::size(a.layerCullDistances); ++i)
            if (a.layerCullDistances[i] != b.layerCullDistances[i]) return false;
        return CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

// levels は入れ子 vector のため Reflect に載らない。
// 比較するのは Inspector で編集できる閾値と Renderer 参照の並びだけ。
// (LODRendererReference::entity は instanceId から解決されるランタイム値)
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

// waypoints と対応する per-waypoint 配列は Reflect に載らない。
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

// layerMaterials は string 配列のため Reflect に載らない。
// heightData / splatData は地形ツールが塗るデータで、Inspector の編集対象ではない
// (ツール側が専用の Undo を持つ)。ここで比較すると巨大配列の走査が入るだけなので除く。
template<>
struct ComponentUndoCompare<scene::TerrainComponent> {
    static bool UserValuesEqual(scene::TerrainComponent& a, scene::TerrainComponent& b)
    {
        return a.layerMaterials == b.layerMaterials
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

// ── 以下は Reflect に載らない vector<struct> を持つが、その中身は専用ツール
//    (Foliage / Detail ブラシ、IK チェーン編集、Terrain Grid の生成) が編集し、
//    それぞれが自前の Undo を持つ。要素数だけを見て、追加・削除は取りこぼさない。
//    WHY 中身まで比較しないか: 要素が数千件になりうる (植生インスタンス・詳細レイヤー)。
//        1 操作ごとに全走査すると Inspector の応答が落ちる。
template<>
struct ComponentUndoCompare<scene::FoliageComponent> {
    static bool UserValuesEqual(scene::FoliageComponent& a, scene::FoliageComponent& b)
    {
        return a.species.size() == b.species.size()
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

template<>
struct ComponentUndoCompare<scene::TerrainDetailComponent> {
    static bool UserValuesEqual(scene::TerrainDetailComponent& a,
                                scene::TerrainDetailComponent& b)
    {
        return a.layers.size() == b.layers.size()
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

template<>
struct ComponentUndoCompare<scene::IKSolverComponent> {
    static bool UserValuesEqual(scene::IKSolverComponent& a, scene::IKSolverComponent& b)
    {
        // IKChain は 15 個以上のフィールドを持つが、Inspector で 1 つ変えるたびに
        // Undo を積むべきなので主要な編集対象を見る。チェーン数の増減も拾う。
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

// cells / cellInstanceIds はグリッド生成の結果で、Inspector から直接は編集しない。
template<>
struct ComponentUndoCompare<scene::TerrainGridComponent> {
    static bool UserValuesEqual(scene::TerrainGridComponent& a, scene::TerrainGridComponent& b)
    {
        return a.cellInstanceIds == b.cellInstanceIds
            && CaptureComponentDigest(a) == CaptureComponentDigest(b);
    }
};

// コンポーネント編集の開始状態を保持する。
// WHY: MSVC 14.51 は関数テンプレート内の依存型を持つローカル構造体で ICE するため、
//      状態型を名前空間スコープへ分離してテンプレートのインスタンス化を単純化する。
template<typename T>
struct ComponentActiveEdit {
    scene::EntityID id;
    ImGuiID activeId = 0;
    T before{};
    bool active = false;
};

// DrawComponentSectionCustom 用 — Snapshot 型を T と分離した版。
// WHY: unique_ptr を持つコンポーネントでは T をそのままスナップショットに使えないため分離する。
//      (T, Snapshot) ペアごとに static スロットが生成されるため MeshCollider / ConvexHull 分離を保証する。
template<typename T, typename Snapshot>
struct ComponentActiveEditCustom {
    scene::EntityID entityId{};
    ImGuiID activeId = 0;
    Snapshot before{};
    bool active = false;
};

// 型付き描画関数を、Undo 処理が受け取る型消去済みコールバックへ橋渡しする。
// WHY: MSVC 14.51 の ICE を避けるため、状態保持とスナップショットを行う重い関数から
//      ラムダ固有型 DrawFn のテンプレート依存を分離する。
template<typename T, typename DrawFn>
void InvokeComponentDraw(T& component, EditorContext& ctx, void* drawFn)
{
    (*static_cast<DrawFn*>(drawFn))(component, ctx);
}

// コンポーネント内部の ImGui 編集を ActiveId の開始から解放まで1操作として記録する。
// WHY: 各 Drag/Slider を個別対応すると記録漏れが生じるため、共通セクションで
//      編集前後のコンポーネント全体をスナップショットする。
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

    // 値が動いていない操作 (参照欄のクリック・折りたたみの開閉など) では積まない。
    if (ComponentUndoCompare<T>::UserValuesEqual(edit.before, component)) {
        edit.active = false;
        return;
    }

    PushComponentValueCommand(
        go, ctx, std::string("Change ") + label, edit.before, component);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    edit.active = false;
}

// MaterialComponent は参照先コンポーネントと MaterialAsset 本体を同じ UI で編集する。
// WHY: 汎用スナップショットを使うと Asset 編集時のキャッシュ reset まで別コマンドになり、
//      MaterialAsset 側の Undo と二重に履歴へ積まれるため、参照パス変更だけを記録する。
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

    // 全スロットの割り当てを Undo 対象にする。判定は汎用側と同じ
    // ComponentUndoCompare を使う (以前はここに同じ比較を手書きで重複させていた)。
    if (!ComponentUndoCompare<scene::MaterialComponent>::UserValuesEqual(
            edit.before, component)) {
        PushComponentValueCommand(
            go, ctx, std::string("Change ") + label, edit.before, component);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }
    edit.active = false;
}

// RigidBodyComponent は shared_ptr の先に編集値を持つため、物理ボディ本体も複製する。
// WHY: Component の浅いコピーだけでは before/after が同じ RigidBody を参照し、
//      Mass や Gravity Scale の Undo が実質的に何も戻さないため。
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

    // 値が動いていない操作 (参照欄のクリック等) では積まない。
    // WHY component の digest だけで足りるか: RigidBodyComponent::Reflect は
    //     rigidBody ポインタの先 (mass / gravityScale 等) まで読み書きする。
    //     Snapshot は body を深いコピーで持つため、before 側の digest も実値を反映する。
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


// Hierarchy パネルからのドラッグ＆ドロップを受け取り、ドロップされた GameObject を返す。
// WHY: IK Solver の Bone 名・Target 名フィールドに Hierarchy から直接ドロップできるようにする。
//      nullptr の場合はドロップなし (BeginDragDropTarget が false を返すか payload 不正)。
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

// カテゴリ別のアクセント色。コンポーネントカードの左帯に使う。
// WHY: Inspector は 10 枚以上のカードが縦に積まれるため、名前を読まないと種類が分からない。
//      系統ごとに色を割り当てておけば、スクロール中でも「緑の帯 = 物理」で目的地を拾える。
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

// 登録テーブルから型 → カテゴリを引く (未登録は Misc)。
// WHY: テンプレート側で ForEachRegisteredComponent を回すと、コンポーネント型ごとに
//      全登録ぶんの実体化が起きて (型数の 2 乗) ビルドが跳ねる。テーブル化は
//      非テンプレート関数に閉じ込め、実体化を 1 回だけに抑える。
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

// 型ごとの帯色。引き当て結果は型ごとの static に畳むので、毎フレームの検索にはならない。
template<typename T>
inline ImU32 ComponentAccent()
{
    static const ImU32 accent = ComponentCategoryAccent(LookupComponentCategory(typeid(T)));
    return accent;
}

// 既存のカテゴリ別 Inspector を一度収集し、GameObject が持つ順序で再生するための一時バッファ。
// WHY: 専用 Inspector 関数を一つへ統合すると、既存の Component 固有 UI と Undo 実装を
//      大規模に書き換える必要がある。描画要求だけを遅延させれば、既存の責務を保ったまま
//      カードの並び順だけを差し替えられる。
struct InspectorComponentDrawCollector {
    struct Request {
        std::string key;
        std::function<void()> draw;
    };

    scene::GameObject* gameObject = nullptr;
    EditorSceneState* editorState = nullptr;
    std::vector<Request> requests;
    bool drawing = false;
    // 収集が「この GameObject の全 Component」を網羅しているときだけ true。
    // WHY: Map モードの絞り込み表示では地形系しか収集されない。そこで残骸掃除まで
    //      走らせると、表示していないだけの Component の並び順を消してしまう。
    bool complete = false;

    void Add(std::string key, std::function<void()> draw)
    {
        requests.push_back({ std::move(key), std::move(draw) });
    }

    void DrawInOrder()
    {
        if (!gameObject) return;

        // 今フレーム実在するキーだけを保存対象に残す (削除された Component の残骸掃除)。
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

        // 描画中のドロップで順序が変更されてもイテレータを無効化しないよう、
        // 並び順はフレーム開始時点のスナップショットを使う。
        const std::vector<std::string> order = editorState
            ? editorState->GetComponentOrder(gameObject->instanceId)
            : std::vector<std::string>{};
        // 保存済み順序に存在する要求を先に描き、後から追加された Component は末尾へ置く。
        for (const std::string& key : order) {
            for (size_t i = 0; i < requests.size(); ++i)
                if (requests[i].key == key) drawRequest(i);
        }
        for (size_t i = 0; i < requests.size(); ++i)
            drawRequest(i);
    }
};

// Component カードの移動を 1 回の Undo 操作として記録する。
// WHY: ドロップ後に別のカードを追加・削除しても GameObject* を保持し続けないよう、
//      Undo/Redo 時は instanceId から対象を引き直す。
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

// エンジン Component カード共通の並び替え指定。
// WHY まとめるか: scope 文字列とドロップ処理を各カードで書き写すと、片方だけ直したときに
//      「一部のカードだけ並び替えできない / 別リストへ落とせてしまう」ズレが生まれる。
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

    // WHY: BoneComponent のような構造上常に有効な補助 Component は enabled を持たない。
    //      共通 Inspector を利用できるよう、bool enabled がある型だけ有効チェックを描画する。
    constexpr bool hasEnabled = requires(T& value) {
        static_cast<bool&>(value.enabled);
    };
    if constexpr (hasEnabled) {
        T beforeEnabled{};
        if (CanRecordEditorUndo(ctx))
            beforeEnabled = *comp;
        header = widgets::ComponentHeader(
            label, accent, &comp->enabled, true,
            MakeComponentReorderTarget(go, ctx, label));
        if (header.enabledChanged) {
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
        if (ImGui::MenuItem("Reset")) {
            const T before = *comp;
            if constexpr (hasEnabled) {
                const bool wasEnabled = comp->enabled;
                *comp = T{};
                comp->enabled = wasEnabled;
            } else {
                *comp = T{};
            }
            PushComponentValueCommand(
                *go, ctx, std::string("Reset ") + label, before, *comp);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Component"))
        {
            compClipboard     = *comp;
            compClipboardType = &typeid(T);
        }
        const bool canPaste = compClipboardType && *compClipboardType == typeid(T);
        if (ImGui::MenuItem("Paste Component Values", nullptr, false, canPaste))
        {
            const T before = *comp;
            if constexpr (hasEnabled) {
                const bool wasEnabled = comp->enabled;
                *comp = std::any_cast<T>(compClipboard);
                comp->enabled = wasEnabled;
            } else {
                *comp = std::any_cast<T>(compClipboard);
            }
            PushComponentValueCommand(
                *go, ctx, std::string("Paste ") + label, before, *comp);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Component"))
            removeRequested = true;
        ImGui::EndPopup();
    }

    if (open) {
        const widgets::ComponentBodyScope body = widgets::BeginComponentBody(header, accent);
        ImGui::Spacing();
        if constexpr (std::is_same_v<T, scene::MaterialComponent>
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
    // カード同士の間隔。詰まっていると帯があっても切れ目が読めない。
    ImGui::Spacing();

    ImGui::PopID();

    if (removeRequested) {
        const T removed = *comp;
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

// EngineコンポーネントのReflect()からInspector本体を自動生成するReflector。
// WHY: enabledはDrawComponentSectionの共通ヘッダーがUndo付きで描画するため、
//      Reflect()内の同名フィールドだけを省き、二重表示を防ぐ。
struct ComponentImGuiReflector final : ImGuiReflector {
    void Field(const char* name, bool& value) override
    {
        if (std::strcmp(name, "enabled") == 0) return;
        ImGuiReflector::Field(name, value);
    }
};

// Reflect()を持つコピー可能コンポーネントを、共通のヘッダー・Undo・本文描画へ接続する。
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

// RegistryカテゴリをEditor表示名へ変換する。
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

// DrawComponentSection の Snapshot カスタマイズ版。
// WHY: unique_ptr を含むコンポーネント (MeshCollider 等) では、コンポーネント全体のコピーが
//      physics body を消去してしまう。CaptureFn / ApplyFn を渡すことで
//      serializable フィールドのみを Undo スナップショットとして保持できる。
//      T が異なれば ComponentActiveEditCustom<T, Snapshot> の static も別インスタンスになる。
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

    // applyFn 経由でコンポーネントへ値を適用し、Undo コマンドを積む共通ヘルパー。
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

    // ヘッダー (有効チェック + 名前 + ⋯ メニュー) はカード表現へ集約済み。
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
            applyFn(*comp, Snapshot{});
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
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Component"))
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
                // 値が動いていない操作 (参照欄のクリック等) では積まない。
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

    // Remove + Undo/Redo : snapchat を使ってコンポーネントを再構築する。
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

// dayNightDriven — SkyRenderer の昼夜カーブがこのライトの色/強度を上書きしている状態。
// WHY 引数で受けるか: 上書きは RenderSystem が毎フレーム行うので、Inspector の値を編集しても
//     画面は変わらない。無効化して出典を書かないと「ライトが壊れている」と読めてしまう。
inline void DrawLightFields(scene::GameObject& go, scene::LightComponent& lc,
                            bool dayNightDriven = false)
{
    static constexpr const char* kTypeNames[] = { "Directional", "Point", "Spot" };
    int typeIdx = static_cast<int>(lc.type);
    if (ImGui::Combo("Type", &typeIdx, kTypeNames, 3))
        lc.type = static_cast<scene::LightComponent::Type>(typeIdx);

    const bool drivenBySky = dayNightDriven && lc.type == scene::LightComponent::Type::Directional;
    ImGui::BeginDisabled(drivenBySky);
    widgets::ColorEdit3("Color", lc.color);
    ImGui::DragFloat("Intensity", &lc.intensity, 0.05f, 0.0f, 200.0f);
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

    if (lc.type != scene::LightComponent::Type::Point) {
        auto fwd = go.transform.forward;
        float dir[3] = { fwd.x, fwd.y, fwd.z };
        ImGui::InputFloat3("Forward", dir, "%.3f", ImGuiInputTextFlags_ReadOnly);
    }

    if (lc.type == scene::LightComponent::Type::Directional) {
        ImGui::Separator();
        ImGui::SeparatorText("Shadow");
        ImGui::Checkbox("Cast Shadows", &lc.castShadows);
        if (lc.castShadows) {
            ImGui::DragFloat("Shadow Strength", &lc.shadowStrength, 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Shadow Bias",     &lc.shadowBias,     0.05f, 0.1f, 10.0f);
            // 0 のとき "Auto" 表示。シーン全体の AABB から自動フィット。
            const char* distFmt = (lc.shadowDistance <= 0.0f) ? "Auto" : "%.1f m";
            ImGui::DragFloat("Shadow Distance", &lc.shadowDistance, 5.0f, 0.0f, 2000.0f, distFmt);
        }
    }
}
inline scene::MeshRenderer CreateDefaultMeshRenderer()
{
    scene::MeshRenderer mr;
    mr.meshPath = "primitive:cube";
    if (auto* resources = renderer::ResourceManager::Active())
        mr.mesh = renderer::PrimitiveMesh::Cube(*resources);
    return mr;
}
inline scene::MaterialComponent CreateDefaultMaterialComponent(bool skinned = false)
{
    scene::MaterialComponent mc;
    (void)skinned;
    return mc;
}
inline scene::AabbColliderComponent CreateAabbCollider(const math::Vector3& size = math::Vector3::ONE)
{
    scene::AabbColliderComponent collider;
    collider.size = size;
    collider.collider = std::make_unique<physics::AABBCollider>(size * 0.5f);
    return collider;
}
inline scene::BoxColliderComponent CreateBoxCollider(const math::Vector3& halfExtents = { 0.5f, 0.5f, 0.5f })
{
    scene::BoxColliderComponent collider;
    collider.size = halfExtents * 2.0f;
    collider.collider = std::make_unique<physics::OBBCollider>(halfExtents);
    return collider;
}
inline scene::SphereColliderComponent CreateSphereCollider(float radius = 0.5f)
{
    scene::SphereColliderComponent collider;
    collider.radius = radius;
    collider.collider = std::make_unique<physics::SphereCollider>(radius);
    return collider;
}
inline scene::CapsuleColliderComponent CreateCapsuleCollider(float radius = 0.5f, float halfHeight = 1.0f)
{
    scene::CapsuleColliderComponent collider;
    collider.radius = radius;
    collider.halfHeight = halfHeight;
    collider.collider = std::make_unique<physics::CapsuleCollider>(radius, halfHeight);
    return collider;
}
inline scene::CylinderColliderComponent CreateCylinderCollider(float radius = 0.5f, float halfHeight = 1.0f)
{
    scene::CylinderColliderComponent collider;
    collider.radius = radius;
    collider.halfHeight = halfHeight;
    collider.collider = std::make_unique<physics::CylinderCollider>(radius, halfHeight);
    return collider;
}
// SyncColliderPreview — Inspector で形状を編集した直後に physics::Collider へ反映する。
// WHY: 以前はここに独自の姿勢反映コピーがあり、しかも world ではなくローカルの
//      position / rotation を使っていたため、親を持つオブジェクトでは Inspector の
//      プレビューとコライダー可視化がずれていた。Engine 側の ColliderSync に一本化する。
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

// WHAT: .mat の assets/ 相対パスを保存 API に渡せるディスクパスへ変換する。
// WHY: MaterialComponent はポータブルな Assets 起点パスだけを保持するため、Editor の保存時だけ projectRoot を補完する。
inline std::string MaterialAssetDiskPath(const EditorContext& ctx, const std::string& assetPath)
{
    return ToProjectAssetDiskPath(ctx.projectRoot, assetPath);
}
inline renderer::Mesh* MeshFromModelPath(const std::string& path, int meshIndex)
{
    if (path.empty()) return nullptr;
    std::string filePath = path;
    int resolvedIndex = meshIndex;
    const size_t slashPos = path.find_last_of('/');
    const size_t searchFrom = slashPos != std::string::npos ? slashPos : 0;
    const size_t colonPos = path.find(':', searchFrom);
    if (colonPos != std::string::npos) {
        std::string suffix = path.substr(colonPos + 1);
        bool allDigits = !suffix.empty();
        for (char c : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            filePath = path.substr(0, colonPos);
            resolvedIndex = std::atoi(suffix.c_str());
        }
    }

    auto* model = asset::AssetManager::LoadModel(filePath);
    if (!model || resolvedIndex < 0 || resolvedIndex >= static_cast<int>(model->meshes.size()))
        return nullptr;
    return model->meshes[static_cast<size_t>(resolvedIndex)].get();
}
inline renderer::Mesh* SourceMeshFromGameObject(scene::GameObject& go,
                                                std::string& outPath,
                                                int& outMeshIndex)
{
    if (auto* mr = go.GetComponent<scene::MeshRenderer>(); mr && mr->mesh) {
        outPath = mr->meshPath;
        outMeshIndex = 0;
        return mr->mesh;
    }

    if (auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>()) {
        if (!smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::LoadModel(smr->modelPath);
        // 1 GameObject = モデル全体になったため、コライダーのソースは
        // Collider 側が持つ meshIndex (呼び出し前に設定済み) の submesh を使う。
        if (smr->model) {
            const size_t submesh = outMeshIndex >= 0 ? static_cast<size_t>(outMeshIndex) : 0u;
            if (submesh < smr->model->meshes.size()) {
                outPath = smr->modelPath;
                outMeshIndex = static_cast<int>(submesh);
                return smr->model->meshes[submesh].get();
            }
        }
    }

    return nullptr;
}
inline std::vector<math::Vector3> MeshPositions(const renderer::Mesh& mesh)
{
    std::vector<math::Vector3> positions;
    positions.reserve(mesh.cpuVertices.size());
    for (const auto& vertex : mesh.cpuVertices)
        positions.push_back(vertex.position);
    return positions;
}
inline bool BuildMeshCollider(scene::MeshColliderComponent& col, const renderer::Mesh* mesh)
{
    if (!mesh || mesh->cpuVertices.empty() || mesh->cpuIndices.empty()) return false;
    col.collider = std::make_unique<physics::TriangleMeshCollider>(MeshPositions(*mesh), mesh->cpuIndices);
    return true;
}
inline bool BuildConvexHullCollider(scene::ConvexHullColliderComponent& col, const renderer::Mesh* mesh)
{
    if (!mesh || mesh->cpuVertices.empty()) return false;
    col.collider = std::make_unique<physics::ConvexHullCollider>(MeshPositions(*mesh));
    return true;
}
inline scene::RigidBodyComponent CreateDefaultRigidBody()
{
    scene::RigidBodyComponent rb;
    rb.rigidBody = std::make_unique<physics::RigidBody>();
    rb.rigidBody->SetMass(1.0f);
    return rb;
}

// コンポーネント追加時の特殊な既定値・依存コンポーネントだけを集約する。
// 新しい単純型は最後のデフォルト経路だけで追加できる。
template<typename T>
void AddRegisteredComponent(scene::GameObject& go)
{
    if constexpr (std::is_same_v<T, scene::MeshRenderer>) {
        go.AddComponent<T>(CreateDefaultMeshRenderer());
        if (!go.GetComponent<scene::MaterialComponent>())
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent());
    } else if constexpr (std::is_same_v<T, scene::SkinnedMeshRenderer>) {
        go.AddComponent<T>();
        if (!go.GetComponent<scene::MaterialComponent>())
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
    } else if constexpr (std::is_same_v<T, scene::MaterialComponent>) {
        go.AddComponent<T>(
            CreateDefaultMaterialComponent(go.GetComponent<scene::SkinnedMeshRenderer>() != nullptr));
    } else if constexpr (std::is_same_v<T, scene::TerrainComponent>) {
        T terrain{};
        terrain.columns = 65;
        terrain.rows = 65;
        terrain.cellSize = 2.0f;
        terrain.maxHeight = 20.0f;
        terrain.chunkSize = 32;
        for (int layer = 0; layer < 4; ++layer)
            terrain.layerMaterials[layer] = DefaultTerrainLayerMaterialPath(layer);
        terrain.InitFlat(0.0f);
        terrain.heightDirty = true;
        terrain.colliderDirty = true;
        go.AddComponent<T>(std::move(terrain));
        if (!go.GetComponent<scene::TerrainColliderComponent>())
            go.AddComponent<scene::TerrainColliderComponent>();
    } else if constexpr (std::is_same_v<T, scene::WaterComponent>) {
        T water{};
        water.resolutionX = 64;
        water.resolutionZ = 64;
        water.extentX = 80.0f;
        water.extentZ = 80.0f;
        water.materialPath = DefaultWaterMaterialPath();
        water.meshDirty = true;
        water.foamDirty = true;
        water.texDirty = true;
        go.AddComponent<T>(std::move(water));
    } else if constexpr (std::is_same_v<T, scene::RigidBodyComponent>) {
        go.AddComponent<T>(CreateDefaultRigidBody());
    } else if constexpr (std::is_same_v<T, scene::AabbColliderComponent>) {
        go.AddComponent<T>(colliderfit::MakeFittedAabbCollider(go));
    } else if constexpr (std::is_same_v<T, scene::BoxColliderComponent>) {
        go.AddComponent<T>(colliderfit::MakeFittedBoxCollider(go));
    } else if constexpr (std::is_same_v<T, scene::SphereColliderComponent>) {
        go.AddComponent<T>(colliderfit::MakeFittedSphereCollider(go));
    } else if constexpr (std::is_same_v<T, scene::CapsuleColliderComponent>) {
        go.AddComponent<T>(colliderfit::MakeFittedCapsuleCollider(go));
    } else if constexpr (std::is_same_v<T, scene::CylinderColliderComponent>) {
        go.AddComponent<T>(colliderfit::MakeFittedCylinderCollider(go));
    } else if constexpr (std::is_same_v<T, scene::MeshColliderComponent>) {
        T collider;
        auto mesh = SourceMeshFromGameObject(go, collider.meshPath, collider.meshIndex);
        BuildMeshCollider(collider, mesh);
        go.AddComponent<T>(std::move(collider));
    } else if constexpr (std::is_same_v<T, scene::ConvexHullColliderComponent>) {
        T collider;
        auto mesh = SourceMeshFromGameObject(go, collider.meshPath, collider.meshIndex);
        BuildConvexHullCollider(collider, mesh);
        go.AddComponent<T>(std::move(collider));
    } else if constexpr (std::is_same_v<T, scene::AnimatorComponent>) {
        go.AddComponent<T>();
        if (!go.GetComponent<scene::SkinnedMeshRenderer>())
            go.AddComponent<scene::SkinnedMeshRenderer>();
        if (!go.GetComponent<scene::MaterialComponent>())
            go.AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
    } else if constexpr (std::is_same_v<T, scene::NavMeshPatrolComponent>) {
        go.AddComponent<T>();
        if (!go.GetComponent<scene::NavMeshAgentComponent>())
            go.AddComponent<scene::NavMeshAgentComponent>();
    } else {
        go.AddComponent<T>();
    }
}

// ある GameObject が現在持っている「登録済みコンポーネント型」の一覧を返す。
// WHY: Add Component の Undo を作るための基準点。追加前後でこの集合を比べれば、
//      依存で一緒に付いたコンポーネントも含めて「増えたぶん」だけが取り出せる。
inline std::vector<std::type_index> CapturePresentComponentTypes(scene::GameObject& go)
{
    std::vector<std::type_index> types;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (go.GetComponent<T>()) types.emplace_back(typeid(T));
    });
    return types;
}

// before に無かった型だけを対象に、Redo 用の「再追加関数」と Undo 用の「削除関数」を集める。
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

        // Redo は「追加直後の値」をそのまま復元する。
        // unique_ptr を含むなどコピーできない型 (MeshCollider 等) は値を持ち運べないため、
        // 既定の追加経路をもう一度走らせる — 追加直後と同じ結果になる。
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

// 登録済みコンポーネントを追加し、この操作で増えた型だけを戻す Undo コマンドを返す。
//
// WHY: 以前はシーン全体を TOML 化して before/after スナップショットにしていた。
//      1 コンポーネント追加のたびに全文シリアライズが 2 回走るうえ、Undo が
//      Scene の Deserialize による全再構築になるため EntityID が振り直され、
//      選択・ロック・エディタ非表示・Inspector のスクロール位置が毎回消えていた。
//      増えたコンポーネントだけを足し引きすれば、シーンの他の部分には一切触れない。
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

    std::vector<std::function<void(scene::GameObject&)>> adders;
    std::vector<std::function<void(scene::GameObject&)>> removers;
    CollectAddedComponentOps(go, before, adders, removers);
    if (adders.empty()) return nullptr;   // 何も増えなかった (既に付いていた)

    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;

    // WHY: GameObject* は Undo までの間に無効化され得るため、instanceId から引き直す。
    auto apply = [scene, instanceId, markDirty](
                     const std::vector<std::function<void(scene::GameObject&)>>& ops) {
        auto* target = scene->FindByGuid(instanceId);
        if (!target) return;
        for (const auto& op : ops) op(*target);
        if (markDirty) markDirty();
    };

    return std::make_unique<LambdaCommand>(
        std::string("Add ") + label,
        [apply, adders]()   { apply(adders); },
        [apply, removers]() { apply(removers); });
}

// 型名 (文字列) で登録済みコンポーネントを追加する。
//
// WHY 名前引きの経路が要るか:
//   FBZZ_REQUIRE_COMPONENT の要求は DLL 境界を越える都合で型名の文字列でしか運べない。
//   ここで ComponentRegistry を畳んで名前を突き合わせ、追加そのものは
//   AddRegisteredComponent<T> へ委ねる。こうすると Fix ボタンで付いたコンポーネントが
//   Add Component メニューから付けたものと完全に同じ初期値になる
//   (コライダーの自動フィット、RigidBody の既定質量、Animator の随伴追加など)。
//   ここで go.AddComponent<T>() を直接呼ぶと、その既定値だけが失われる。
inline bool AddRegisteredComponentByName(scene::GameObject& go, std::string_view typeName)
{
    bool handled = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (handled) return;
        if (!Registration::addable) return;              // 内部型は手で足せない
        if (typeName != Registration::serializedName) return;
        if (!go.GetComponent<T>()) AddRegisteredComponent<T>(go);
        handled = true;
    });
    return handled;
}

// 不足している必須コンポーネントをまとめて追加し、この操作で増えた型だけを戻す
// Undo コマンドを返す。Inspector の "Fix" ボタンの実体。
//
// WHY 1 コマンドにまとめるか: 3 個足りない状態を直したとき、Ctrl+Z 3 回で戻るのは
//     「1 回のクリックを 1 回で取り消せる」という期待に反する。押した操作の単位で戻す。
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

    std::vector<std::function<void(scene::GameObject&)>> adders;
    std::vector<std::function<void(scene::GameObject&)>> removers;
    CollectAddedComponentOps(go, before, adders, removers);
    if (adders.empty()) return nullptr;

    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;

    // WHY: GameObject* は Undo までの間に無効化され得るため、instanceId から引き直す。
    auto apply = [scene, instanceId, markDirty](
                     const std::vector<std::function<void(scene::GameObject&)>>& ops) {
        auto* target = scene->FindByGuid(instanceId);
        if (!target) return;
        for (const auto& op : ops) op(*target);
        if (markDirty) markDirty();
    };

    return std::make_unique<LambdaCommand>(
        "Add Missing Components",
        [apply, adders]()   { apply(adders); },
        [apply, removers]() { apply(removers); });
}

// スクリプトを 1 件追加し、その 1 件だけを戻す Undo コマンドを返す。
// WHY: スクリプト追加は ScriptComponent::scripts への要素追加であり、
//      「型が増えたか」を見る汎用差分では ScriptComponent が既にある場合を検出できない。
inline std::unique_ptr<ICommand> AddScriptWithUndo(scene::GameObject& go,
                                                   EditorContext& ctx,
                                                   const std::string& typeName)
{
    auto script = scene::ScriptFactory::Create(typeName);
    if (!script) return nullptr;
    script->SetContext(ctx.activeScene, &go);
    script->Reset();
    script->OnValidate();

    // この操作で ScriptComponent 自体も新設したかを覚えておく (Undo でそこまで戻すため)。
    const bool hadComponent = go.GetComponent<scene::ScriptComponent>() != nullptr;
    auto* sc = go.GetComponent<scene::ScriptComponent>();
    if (!sc) sc = &go.AddComponent<scene::ScriptComponent>();

    scene::ScriptEntry& entry = sc->scripts.emplace_back();
    entry.script = std::move(script);
    const std::size_t addedIndex = sc->scripts.size() - 1;

    if (!(CanRecordEditorUndo(ctx) && ctx.activeScene)) return nullptr;

    scene::Scene* scene = ctx.activeScene;
    const std::string instanceId = go.instanceId;
    const auto markDirty = ctx.markSceneDirty;

    return std::make_unique<LambdaCommand>(
        std::string("Add Script ") + typeName,
        [scene, instanceId, typeName, markDirty]() {
            auto* target = scene->FindByGuid(instanceId);
            if (!target) return;
            auto newScript = scene::ScriptFactory::Create(typeName);
            if (!newScript) return;
            auto* comp = target->GetComponent<scene::ScriptComponent>();
            if (!comp) comp = &target->AddComponent<scene::ScriptComponent>();
            newScript->SetContext(scene, target);
            newScript->Reset();
            newScript->OnValidate();
            comp->scripts.emplace_back().script = std::move(newScript);
            if (markDirty) markDirty();
        },
        [scene, instanceId, addedIndex, hadComponent, markDirty]() {
            auto* target = scene->FindByGuid(instanceId);
            if (!target) return;
            auto* comp = target->GetComponent<scene::ScriptComponent>();
            if (!comp) return;
            if (addedIndex < comp->scripts.size())
                comp->scripts.erase(comp->scripts.begin()
                                    + static_cast<std::ptrdiff_t>(addedIndex));
            if (!hadComponent && comp->scripts.empty())
                target->RemoveComponent<scene::ScriptComponent>();
            if (markDirty) markDirty();
        });
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
// Add Component メニュー。targets が複数なら、選択全体へまとめて追加する。
//
// WHY: 「選んだ 20 個全部に AudioSource を足す」はレベル調整で普通に出る操作なのに、
//      この UI が単一 GameObject 前提だったため 20 回繰り返すしかなかった。
//      対象をリストで受ければ、単体は「要素 1 個のリスト」として同じ経路に乗る。
//      追加は 1 回の Undo でまとめて戻る (対象数だけ Ctrl+Z を叩かせない)。
inline void DrawAddComponentMenuMulti(const std::vector<scene::GameObject*>& targets,
                                      char (&filterBuffer)[64],
                                      EditorContext& ctx,
                                      const char* buttonLabel)
{
    if (targets.empty()) return;
    scene::GameObject& go = *targets.front();   // フィルタ表示や単体経路の基準

    // Add Component は Inspector で最も押されるボタン。コンポーネントカードの列が
    // 続いた後に地味な既定色で置くと底に埋もれるため、アクセント色 + 1 段高い枠にする。
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

    // 全対象へ addOne を適用し、返ってきた Undo コマンドを 1 つにまとめる。
    // WHY: 対象ごとにコマンドを積むと、20 個へ足した操作を戻すのに Ctrl+Z を
    //      20 回叩くことになる。ユーザー視点の 1 操作は 1 コマンドに保つ。
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

    // perform() は追加を実行し、その操作を戻す Undo コマンド (不要なら nullptr) を返す。
    // WHY: 「何が増えたか」を知っているのは呼び出し側なので、差分の作り方はそちらに任せる。
    //      addItem 側はフィルタ照合とメニュー項目の描画だけに責務を絞る。
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
                    // 1 体でも未所持なら追加できる (所持済みの対象は飛ばす)。
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

    anyShown |= AddComponentCategory("Scripts", filter, [&](const char* category, const char*) {
        bool shown = false;
        const auto scriptTypeNames = scene::ScriptFactory::RegisteredTypeNames();
        for (const std::string& typeName : scriptTypeNames) {
            shown |= addItem(category, typeName.c_str(), true, [&]() {
                return AddToAllTargets([&](scene::GameObject& target) {
                    return AddScriptWithUndo(target, ctx, typeName);
                }, "Add Script " + typeName);
            });
        }
        return shown;
    });

    if (!anyShown)
        ImGui::TextDisabled("No results");

    ImGui::EndPopup();

    if (didAdd)
        filterBuffer[0] = '\0';
}

// 単一 GameObject 版 (要素 1 個のリストとして同じ経路へ乗せる)。
inline void DrawAddComponentMenu(scene::GameObject& go, char (&filterBuffer)[64], EditorContext& ctx)
{
    DrawAddComponentMenuMulti({ &go }, filterBuffer, ctx, "Add Component");
}

// widgets::DragVec3 の 2 成分版。ラベル列・軸色を Vector3 の行と揃えるため同じ構成で描く。
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
// スクリプトカードを Component 並びへ個別登録するための描画ライフサイクル。
// Begin/End は 1 GameObject の全カードを囲み、DrawScriptCard は 1 枚だけ描く。
std::string GetScriptOrderKey(const scene::ScriptComponent& component, int index);
void BeginScriptInspectorFrame(scene::GameObject* go, EditorContext& ctx);
void DrawScriptCard(scene::GameObject* go, EditorContext& ctx, int index);
void EndScriptInspectorFrame(scene::GameObject* go, EditorContext& ctx);
void DrawScriptInspectors(scene::GameObject* go, EditorContext& ctx);

} // namespace fbzz::editor
