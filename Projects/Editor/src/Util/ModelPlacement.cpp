// FBZZ Engine
// ModelPlacement.cpp | fbzz::editor
// .fzasset アセットから Scene 用 GameObject 階層を構築する
#include <Editor/Util/ModelPlacement.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <cctype>
#include <filesystem>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

std::string SanitizeMaterialFileName(std::string name, uint32_t fallbackIndex)
{
    if (name.empty())
        name = "Material_" + std::to_string(fallbackIndex);

    for (char& c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
            c = '_';
    }
    if (name == "." || name == "..")
        name = "Material_" + std::to_string(fallbackIndex);
    return name;
}

std::string ResolveImportedMaterialPath(const std::string& modelPath,
                                        const asset::ModelAsset* modelAsset,
                                        int meshIndex)
{
    if (!modelAsset || modelAsset->lods.empty() || meshIndex < 0)
        return "Assets/Materials/Fallback/FallbackSkinned.mat";

    const auto& submeshes = modelAsset->lods[0].submeshes;
    if (meshIndex >= static_cast<int>(submeshes.size()))
        return "Assets/Materials/Fallback/FallbackSkinned.mat";

    const uint32_t slotIndex = submeshes[static_cast<size_t>(meshIndex)].materialSlotIndex;
    if (slotIndex >= modelAsset->materialSlotNames.size())
        return "Assets/Materials/Fallback/FallbackSkinned.mat";

    const std::filesystem::path modelFilePath =
        util::FileSystem::PathFromUtf8(modelPath);
    const std::filesystem::path modelDir = modelFilePath.parent_path();
    std::string fileStem = SanitizeMaterialFileName(
        modelAsset->materialSlotNames[slotIndex], slotIndex);
    uint32_t duplicateCount = 0;
    for (uint32_t i = 0; i < slotIndex; ++i) {
        if (SanitizeMaterialFileName(modelAsset->materialSlotNames[i], i) == fileStem)
            ++duplicateCount;
    }
    if (duplicateCount > 0)
        fileStem += "_" + std::to_string(duplicateCount);
    // import パイプラインは Foo/Foo.fzasset と Foo/materials/*.mat を同じ生成物フォルダに置く。
    // WHY: モデル本体と従属アセットを Foo/ に閉じ込め、移動・削除・再 import の単位を明確にする。
    std::filesystem::path matFsPath =
        modelDir / "materials" / (fileStem + ".mat");
    if (!util::FileSystem::Exists(matFsPath))
        return "Assets/Materials/Fallback/FallbackSkinned.mat";

    std::string normalized = util::FileSystem::NormalizePathSeparators(
        util::FileSystem::PathToUtf8(matFsPath));
    const std::string lower = util::StringUtils::ToLower(normalized);
    const size_t assetsPos = lower.rfind("/assets/");
    if (assetsPos != std::string::npos)
        return normalized.substr(assetsPos + 1);
    if (lower.starts_with("assets/"))
        return normalized;
    return normalized;
}

void LinkBoneToRenderers(std::vector<scene::SkinnedMeshRenderer*>& renderers,
                         int nodeIndex,
                         scene::EntityID boneEntity,
                         scene::EntityID skeletonRootEntity,
                         const asset::Skeleton& skeleton)
{
    for (auto* smr : renderers) {
        if (!smr) continue;
        if (smr->nodeEntities.size() != skeleton.nodes.size())
            smr->nodeEntities.assign(skeleton.nodes.size(), scene::EntityID::INVALID);
        smr->nodeEntities[static_cast<size_t>(nodeIndex)] = boneEntity;
        if (nodeIndex == skeleton.rootNodeIndex)
            smr->skeletonRootEntity = skeletonRootEntity;
    }
}

scene::EntityID CreateBoneHierarchyNode(scene::Scene& scene,
                                        scene::GameObject& owner,
                                        scene::GameObject& parent,
                                        std::vector<scene::SkinnedMeshRenderer*>& renderers,
                                        const asset::Skeleton& skeleton,
                                        int nodeIndex,
                                        std::vector<scene::EntityID>& createdNodes)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size()))
        return scene::EntityID::INVALID;

    if (createdNodes[static_cast<size_t>(nodeIndex)] != scene::EntityID::INVALID)
        return createdNodes[static_cast<size_t>(nodeIndex)];

    const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    auto& boneObject = scene.CreateGameObject(node.name.empty() ? "Bone" : node.name);
    boneObject.layer = owner.layer;
    boneObject.transform.position = node.bindTranslation;
    boneObject.transform.rotation = node.bindRotation;
    boneObject.transform.scale    = node.bindScale;
    boneObject.SetParent(parent);

    scene::BoneComponent bone{};
    bone.boneName = node.name;
    bone.nodeIndex = nodeIndex;
    bone.boneIndex = node.boneIndex;
    bone.skinnedMeshEntity = owner.GetID();
    bone.generated = true;
    boneObject.AddComponent<scene::BoneComponent>(std::move(bone));

    const scene::EntityID boneEntity = boneObject.GetID();
    createdNodes[static_cast<size_t>(nodeIndex)] = boneEntity;

    const scene::EntityID skeletonRootEntity =
        (nodeIndex == skeleton.rootNodeIndex) ? boneEntity : scene::EntityID::INVALID;
    LinkBoneToRenderers(renderers, nodeIndex, boneEntity, skeletonRootEntity, skeleton);

    for (int childIndex : node.children)
        CreateBoneHierarchyNode(scene, owner, boneObject, renderers, skeleton, childIndex, createdNodes);

    return boneEntity;
}

void CreateBoneHierarchyForModel(scene::Scene& scene,
                                 scene::GameObject& owner,
                                 std::vector<scene::SkinnedMeshRenderer*>& renderers,
                                 const asset::Model& model)
{
    if (!model.skeleton || model.skeleton->nodes.empty() || renderers.empty())
        return;

    const asset::Skeleton& skeleton = *model.skeleton;
    std::vector<scene::EntityID> createdNodes(
        skeleton.nodes.size(), scene::EntityID::INVALID);

    if (skeleton.rootNodeIndex >= 0 &&
        skeleton.rootNodeIndex < static_cast<int>(skeleton.nodes.size())) {
        const scene::EntityID rootEntity = CreateBoneHierarchyNode(
            scene, owner, owner, renderers, skeleton, skeleton.rootNodeIndex, createdNodes);
        for (auto* smr : renderers)
            if (smr) smr->skeletonRootEntity = rootEntity;
    }

    // WHY: 一部 DCC は rootNodeIndex から到達できない補助ノードを含むため、
    //      未生成ノードも parentIndex を見て階層へ接続する。
    for (int nodeIndex = 0; nodeIndex < static_cast<int>(skeleton.nodes.size()); ++nodeIndex) {
        if (createdNodes[static_cast<size_t>(nodeIndex)] != scene::EntityID::INVALID)
            continue;
        const int parentIndex = skeleton.nodes[static_cast<size_t>(nodeIndex)].parentIndex;
        scene::GameObject* parent = &owner;
        if (parentIndex >= 0) {
            const scene::EntityID parentEntity = CreateBoneHierarchyNode(
                scene, owner, owner, renderers, skeleton, parentIndex, createdNodes);
            if (auto* parentObject = scene.GetGameObject(parentEntity))
                parent = parentObject;
        }
        CreateBoneHierarchyNode(scene, owner, *parent, renderers, skeleton, nodeIndex, createdNodes);
    }
}

} // namespace

scene::EntityID SpawnModelAssetHierarchy(EditorContext& ctx,
                                         const std::string& modelPath,
                                         const math::Vector3* worldPosition,
                                         scene::EntityID parentId)
{
    if (!ctx.activeScene) return scene::EntityID::INVALID;

    auto* model = asset::AssetManager::LoadModel(modelPath);
    const auto modelAssetHandle = asset::AssetManager::Load<asset::ModelAsset>(modelPath);
    const auto* modelAsset = asset::AssetManager::Get(modelAssetHandle);
    if (!model || model->meshes.empty()) {
        FBZZ_LOG_ERROR("ModelPlacement: ModelAsset load failed [%s]", modelPath.c_str());
        return scene::EntityID::INVALID;
    }

    scene::GameObject* parent = nullptr;
    if (parentId != scene::EntityID::INVALID)
        parent = ctx.activeScene->GetGameObject(parentId);

    const std::string stemName =
        util::FileSystem::PathFromUtf8(modelPath).stem().string();
    auto& root = ctx.activeScene->CreateGameObject(stemName.empty() ? "Model" : stemName);
    if (worldPosition)
        root.transform.position = *worldPosition;
    if (parent)
        root.SetParent(parent);

    const int meshCount = static_cast<int>(model->meshes.size());
    std::vector<scene::SkinnedMeshRenderer*> renderers;
    renderers.reserve(static_cast<size_t>(meshCount));

    auto addRenderer = [&](scene::GameObject& target, int meshIndex) {
        scene::SkinnedMeshRenderer smr;
        smr.modelPath = modelPath;
        smr.meshIndex = meshIndex;
        smr.model     = model;
        auto& renderer = target.AddComponent<scene::SkinnedMeshRenderer>(std::move(smr));
        renderers.push_back(&renderer);

        // .fzasset はジオメトリの実体だけを持つため、配置直後に見える最低限の既定材を割り当てる。
        // WHY: MaterialComponent が空だと GeometryPass が描画をスキップするため、
        //      D&D した結果がユーザーに見えない状態になる。
        scene::MaterialComponent mc;
        mc.materialPath = ResolveImportedMaterialPath(modelPath, modelAsset, meshIndex);
        target.AddComponent<scene::MaterialComponent>(std::move(mc));
    };

    if (meshCount == 1) {
        addRenderer(root, 0);
        CreateBoneHierarchyForModel(*ctx.activeScene, root, renderers, *model);
        return root.GetID();
    }

    for (int meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        auto& child = ctx.activeScene->CreateGameObject(
            root.name + "_Mesh" + std::to_string(meshIndex));
        child.SetParent(root);
        addRenderer(child, meshIndex);
    }

    CreateBoneHierarchyForModel(*ctx.activeScene, root, renderers, *model);
    return root.GetID();
}

} // namespace fbzz::editor
