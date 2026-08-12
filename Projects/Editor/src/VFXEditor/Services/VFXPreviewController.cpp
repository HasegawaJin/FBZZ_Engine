// FBZZ Engine
// VFXPreviewController.cpp | fbzz::editor
// 専用 Preview World の再生制御・環境設定・同時プレビュー複製の実装
#include <Editor/VFXEditor/Services/VFXPreviewController.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

std::vector<scene::EntityID> VFXPreviewController::BuildEffectGroup(EditorContext& ctx) const
{
    std::vector<scene::EntityID> group;
    if (!ctx.activeScene)
        return group;
    const scene::EntityID sel = ctx.PrimarySelected();
    if (!sel.IsValid())
        return group;
    auto* selGo = ctx.activeScene->GetGameObject(sel);
    if (!selGo || !selGo->GetComponent<scene::ParticleEmitter>())
        return group;

    // 選択エミッターを起点に、子 GameObject と SubEmitter 名前参照を幅優先で辿る。
    // WHY: SubEmitter は GameObject 名参照なので循環し得る。visited で一度だけ処理する。
    std::vector<scene::EntityID> visited;
    std::vector<scene::EntityID> stack{ sel };
    while (!stack.empty()) {
        const scene::EntityID id = stack.back();
        stack.pop_back();
        if (Contains(visited, id))
            continue;
        visited.push_back(id);
        auto* go = ctx.activeScene->GetGameObject(id);
        if (!go)
            continue;
        auto* emitter = go->GetComponent<scene::ParticleEmitter>();
        if (emitter)
            group.push_back(id);
        // エミッターを持たない中間ノードの下にもエミッターが居られるよう、子は常に辿る
        for (int i = 0; i < go->GetChildCount(); ++i)
            if (auto* child = go->GetChild(i))
                stack.push_back(child->GetID());
        if (emitter) {
            const std::string* refs[] = {
                &emitter->birthSubEmitter, &emitter->deathSubEmitter, &emitter->collisionSubEmitter
            };
            for (const std::string* name : refs) {
                if (name->empty()) continue;
                if (auto* target = ctx.activeScene->Find(*name))
                    stack.push_back(target->GetID());
            }
        }
    }
    return group;
}


void VFXPreviewController::RequestScrub(EditorContext& ctx,
                                  const std::vector<scene::EntityID>& group, float targetTime)
{
    actorTime = (std::max)(targetTime, 0.0f);
    if (ctx.vfxPreviewScene != nullptr) {
        if (auto* graph = ctx.vfxPreviewScene->GetComponent<scene::VFXGraphComponent>(
                graphEntity))
            graph->editorScrubTime = actorTime;
        if (auto* actor = ctx.vfxPreviewScene->GetGameObject(actorEntity)) {
            if (auto* animator = actor->GetComponent<scene::AnimatorComponent>()) {
                animator->playing = false;
                animator->time = actorTime;
                animator->stateTime = actorTime;
                animator->previousEventTime = actorTime;
            }
        }
    }
    if (!ctx.activeScene) return;
    // グループ全体へ同じ「エフェクト時刻」を要求することで、SubEmitter 連鎖もまとめて巻き戻す。
    for (const scene::EntityID id : group)
        if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
            emitter->editorScrubTime = (std::max)(targetTime, 0.0f);
}

bool VFXPreviewController::LoadPreviewAsset(EditorContext& ctx, const std::string& path,
                                            std::string* outError)
{
    if (ctx.vfxPreviewScene == nullptr) {
        if (outError) *outError = "Preview Sceneが初期化されていません";
        return false;
    }
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    auto* actor = ctx.vfxPreviewScene->GetGameObject(actorEntity);
    if (extension == ".fbx" || extension == ".gltf" || extension == ".glb") {
        if (actor != nullptr) (void)ctx.vfxPreviewScene->DestroyGameObject(actorEntity);
        actor = &ctx.vfxPreviewScene->CreateGameObject("__VFX_PREVIEW_ACTOR");
        actorEntity = actor->GetID();
        actorModelPath = path;
        actorControllerPath.clear();
        actorClipPath.clear();
        actorMaterialPath.clear();
        scene::SkinnedMeshRenderer renderer;
        renderer.modelPath = path;
        renderer.model = asset::AssetManager::LoadModel(path);
        if (renderer.model == nullptr) {
            if (outError) *outError = "モデルを読み込めません: " + path;
            (void)ctx.vfxPreviewScene->DestroyGameObject(actorEntity);
            actorEntity = scene::EntityID::INVALID;
            return false;
        }
        actor->AddComponent<scene::SkinnedMeshRenderer>(std::move(renderer));
        scene::AnimatorComponent animator;
        animator.clipSources.push_back(path);
        animator.loop = actorLoop;
        animator.speed = actorSpeed;
        actor->AddComponent<scene::AnimatorComponent>(std::move(animator));
        actor->AddComponent<scene::MaterialComponent>();
        RefreshActorBones(ctx);
        ApplyActorAttachment(ctx);
        return true;
    }
    if (actor == nullptr) {
        if (outError) *outError = "先にFBXモデルをPreviewへドロップしてください";
        return false;
    }
    auto* animator = actor->GetComponent<scene::AnimatorComponent>();
    if (extension == ".anim") {
        if (animator == nullptr) animator = &actor->AddComponent<scene::AnimatorComponent>();
        actorClipPath = path;
        if (std::find(animator->clipSources.begin(), animator->clipSources.end(), path)
            == animator->clipSources.end())
            animator->clipSources.push_back(path);
        animator->clipsLoaded = false;
        animator->playing = actorPlaying;
        return true;
    }
    if (extension == ".animcontroller" || extension == ".animctrl") {
        if (animator == nullptr) animator = &actor->AddComponent<scene::AnimatorComponent>();
        actorControllerPath = path;
        animator->controllerPath = path;
        asset::AnimatorControllerAsset controller;
        if (asset::LoadAnimatorControllerAsset(path, controller)) {
            asset::ApplyAnimatorControllerAsset(controller, *animator);
            animator->loadedControllerPath = path;
            if (!actorState.empty()) animator->currentStateName = actorState;
        } else {
            animator->loadedControllerPath.clear();
        }
        animator->playing = actorPlaying;
        return true;
    }
    if (extension == ".mat") {
        auto* material = actor->GetComponent<scene::MaterialComponent>();
        if (material == nullptr) material = &actor->AddComponent<scene::MaterialComponent>();
        material->materialPath = path;
        actorMaterialPath = path;
        return true;
    }
    if (outError) *outError = "Preview Actorへ適用できない形式です: " + path;
    return false;
}

void VFXPreviewController::RefreshActorBones(EditorContext& ctx)
{
    actorBones.clear();
    if (ctx.vfxPreviewScene == nullptr) return;
    scene::GameObject* actor = ctx.vfxPreviewScene->GetGameObject(actorEntity);
    if (actor == nullptr) return;
    std::vector<scene::GameObject*> pending{ actor };
    while (!pending.empty()) {
        scene::GameObject* current = pending.back();
        pending.pop_back();
        if (const auto* bone = current->GetComponent<scene::BoneComponent>())
            actorBones.push_back(bone->boneName);
        for (int index = 0; index < current->GetChildCount(); ++index)
            if (auto* child = current->GetChild(index)) pending.push_back(child);
    }
    std::sort(actorBones.begin(), actorBones.end());
    actorBones.erase(std::unique(actorBones.begin(), actorBones.end()), actorBones.end());
}

void VFXPreviewController::ApplyActorAttachment(EditorContext& ctx)
{
    if (ctx.vfxPreviewScene == nullptr) return;
    auto* graph = ctx.vfxPreviewScene->GetGameObject(graphEntity);
    auto* actor = ctx.vfxPreviewScene->GetGameObject(actorEntity);
    if (graph == nullptr || actor == nullptr) return;
    scene::GameObject* target = actor;
    if (attachGraphToBone && !selectedBone.empty()) {
        std::vector<scene::GameObject*> pending{ actor };
        while (!pending.empty()) {
            scene::GameObject* current = pending.back();
            pending.pop_back();
            const auto* bone = current->GetComponent<scene::BoneComponent>();
            if (bone != nullptr && bone->boneName == selectedBone) {
                target = current;
                break;
            }
            for (int index = 0; index < current->GetChildCount(); ++index)
                if (auto* child = current->GetChild(index)) pending.push_back(child);
        }
    }
    graph->SetParent(*target);
    graph->transform.position = math::Vector3::ZERO;
}


void VFXPreviewController::SyncInstanceCopies(EditorContext& ctx, const std::string& assetPath)
{
    if (ctx.vfxPreviewScene == nullptr || assetPath.empty()) return;
    const int desired = std::clamp(instanceCount, 1, 64) - 1;  // ルートを除いた追加分
    const int current = static_cast<int>(copyEntities.size());
    if (desired == current && copySpread == instanceSpread) return;

    // 数が変わったら作り直す。差分更新にすると配置の欠番を管理する必要が出て、
    // 「消したはずのコピーが残る」類の状態バグを抱え込むため。
    for (const scene::EntityID id : copyEntities)
        (void)ctx.vfxPreviewScene->DestroyGameObject(id);
    copyEntities.clear();
    copySpread = instanceSpread;

    for (int index = 0; index < desired; ++index) {
        auto& copy = ctx.vfxPreviewScene->CreateGameObject(
            "__VFX_PREVIEW_COPY_" + std::to_string(index));
        // 円周上へ均等配置する。格子だと手前と奥で重なり方が偏り、
        // 「実戦密度での重なり」を見る目的に合わない。
        const float angle = 6.28318530718f * static_cast<float>(index)
            / static_cast<float>((std::max)(desired, 1));
        copy.transform.position = {
            std::cos(angle) * instanceSpread,
            0.0f,
            std::sin(angle) * instanceSpread
        };
        scene::VFXGraphComponent component;
        component.graphPath = assetPath;
        component.playOnAwake = false;
        component.playing = true;
        component.editorPreviewFrame = Time::frameCount;
        copy.AddComponent<scene::VFXGraphComponent>(std::move(component));
        copyEntities.push_back(copy.GetID());
    }
}


void VFXPreviewController::ApplyEnvironmentPreset(int presetIndex)
{
    // 制作中に切り替えたくなる代表的な 4 種。
    // 「暗所で作って昼のマップで破綻する」を防ぐのが目的なので、
    // 明るさの幅 (夜〜屋外昼) と、色被りのない中間 (Neutral Gray) を必ず含める。
    VFXPreviewEnvironment environment;
    switch (presetIndex) {
    case 1: // Daylight — 屋外昼。最も破綻が出やすい条件
        environment.name = "Daylight";
        environment.backgroundColor = { 0.45f, 0.58f, 0.75f, 1.0f };
        environment.lightDirection = { -0.35f, -0.9f, -0.25f };
        environment.lightColor = { 1.0f, 0.97f, 0.9f };
        environment.lightIntensity = 3.0f;
        environment.ambientColor = { 0.35f, 0.4f, 0.48f };
        environment.showFloorGrid = true;
        break;
    case 2: // Interior — 室内。減衰した暖色の環境光
        environment.name = "Interior";
        environment.backgroundColor = { 0.08f, 0.075f, 0.07f, 1.0f };
        environment.lightDirection = { -0.2f, -0.85f, -0.5f };
        environment.lightColor = { 1.0f, 0.9f, 0.75f };
        environment.lightIntensity = 1.2f;
        environment.ambientColor = { 0.09f, 0.08f, 0.07f };
        environment.showFloorGrid = true;
        break;
    case 3: // Neutral Gray — 色被りのない基準。色設計の判断に使う
        environment.name = "Neutral Gray";
        environment.backgroundColor = { 0.22f, 0.22f, 0.22f, 1.0f };
        environment.lightColor = { 1.0f, 1.0f, 1.0f };
        environment.lightIntensity = 1.5f;
        environment.ambientColor = { 0.18f, 0.18f, 0.18f };
        environment.showFloorGrid = true;
        break;
    case 0:
    default: // Dark — 発光の見え方を確認する既定
        environment.name = "Dark";
        break;
    }
    environment = environment;
    showFloorGrid = environment.showFloorGrid;
    ++environmentRevision;
}

} // namespace fbzz::editor
