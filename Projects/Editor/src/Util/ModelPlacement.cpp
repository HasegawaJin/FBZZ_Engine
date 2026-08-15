// FBZZ Engine
// ModelPlacement.cpp | fbzz::editor
// .fbx アセットから Scene 用 GameObject 階層を構築する
#include <Editor/Util/ModelPlacement.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
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
                                        int meshIndex,
                                        bool skinned)
{
    const char* fallbackPath = skinned
        ? "Assets/Materials/Fallback/FallbackSkinned.mat"
        : "Assets/Materials/Fallback/Fallback.mat";
    if (!modelAsset || modelAsset->lods.empty() || meshIndex < 0)
        return fallbackPath;

    const auto& submeshes = modelAsset->lods[0].submeshes;
    if (meshIndex >= static_cast<int>(submeshes.size()))
        return fallbackPath;

    const uint32_t slotIndex = submeshes[static_cast<size_t>(meshIndex)].materialSlotIndex;
    if (slotIndex >= modelAsset->materialSlotNames.size())
        return fallbackPath;

    const std::filesystem::path modelFilePath =
        util::FileSystem::PathFromUtf8(modelPath);
    const std::filesystem::path modelDir =
        util::StringUtils::ToLower(util::FileSystem::GetExtension(modelPath)) == ".fbx"
        ? modelFilePath.parent_path() / modelFilePath.stem()
        : modelFilePath.parent_path();
    std::string fileStem = SanitizeMaterialFileName(
        modelAsset->materialSlotNames[slotIndex], slotIndex);
    uint32_t duplicateCount = 0;
    for (uint32_t i = 0; i < slotIndex; ++i) {
        if (SanitizeMaterialFileName(modelAsset->materialSlotNames[i], i) == fileStem)
            ++duplicateCount;
    }
    if (duplicateCount > 0)
        fileStem += "_" + std::to_string(duplicateCount);
    // import パイプラインは Foo.fbx の隣に Foo/materials/*.mat を置く。
    // WHY: Scene 参照は原本 .fbx に固定し、ユーザー編集対象の従属アセットだけを Assets 側に残すため。
    std::filesystem::path matFsPath =
        modelDir / "materials" / (fileStem + ".mat");
    if (!util::FileSystem::Exists(matFsPath))
        return fallbackPath;

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

std::string ResolveImportedMeshName(const std::string& modelPath,
                                    const asset::ModelAsset* modelAsset,
                                    int meshIndex)
{
    if (modelAsset && !modelAsset->lods.empty() && meshIndex >= 0) {
        const auto& submeshes = modelAsset->lods[0].submeshes;
        if (meshIndex < static_cast<int>(submeshes.size()) &&
            !submeshes[static_cast<size_t>(meshIndex)].name.empty())
            return submeshes[static_cast<size_t>(meshIndex)].name;
    }

    // v2 以前の .fzasset は名前を持たないため、FBX 原本を読んで互換的に補完する。
    if (util::StringUtils::ToLower(util::FileSystem::GetExtension(modelPath)) == ".fbx") {
        const FbxScanResult scan =
            FbxImportTool::Scan(asset::AssetManager::ResolveAssetPath(modelPath));
        if (scan.valid && meshIndex >= 0 && meshIndex < static_cast<int>(scan.meshNames.size()) &&
            !scan.meshNames[static_cast<size_t>(meshIndex)].empty())
            return scan.meshNames[static_cast<size_t>(meshIndex)];
    }

    return "Mesh_" + std::to_string(meshIndex);
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
    std::vector<std::string> meshNames;
    meshNames.reserve(static_cast<size_t>(meshCount));
    for (int meshIndex = 0; meshIndex < meshCount; ++meshIndex)
        meshNames.push_back(ResolveImportedMeshName(modelPath, modelAsset, meshIndex));
    if (meshCount == 1)
        root.name = meshNames[0];

    std::vector<scene::SkinnedMeshRenderer*> skinnedRenderers;
    skinnedRenderers.reserve(static_cast<size_t>(meshCount));

    // スキンドメッシュを 1 つでも含むモデルは、1 GameObject でモデル全体を描く。
    // WHY: SkinnedMeshRenderer は submesh 指定を持たず常にモデル全体を描画するため、
    //      submesh ごとに子 GO を作ると同じモデルが枚数分重なって描かれてしまう。
    //      submesh ごとの .mat は MaterialComponent のスロット配列で表現する。
    const bool anySkinned = [&] {
        for (const auto& mesh : model->meshes)
            if (mesh && mesh->isSkinned) return true;
        return false;
    }();

    if (anySkinned) {
        scene::SkinnedMeshRenderer smr;
        smr.modelPath = modelPath;
        smr.model     = model;
        skinnedRenderers.push_back(&root.AddComponent<scene::SkinnedMeshRenderer>(std::move(smr)));

        // submesh ごとに 1 スロット。スロット i は model->meshes[i] に対応する。
        // WHY: MaterialComponent が空だと GeometryPass が描画をスキップするため、
        //      D&D した結果がユーザーに見えない状態になる。既定材を必ず割り当てる。
        scene::MaterialComponent mc;
        mc.ResizeSlots(static_cast<size_t>(meshCount));
        for (int meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
            const renderer::Mesh* mesh = model->meshes[static_cast<size_t>(meshIndex)].get();
            mc.RawSlotAt(static_cast<size_t>(meshIndex)).materialPath =
                ResolveImportedMaterialPath(modelPath, modelAsset, meshIndex,
                                            mesh && mesh->isSkinned);
        }
        root.AddComponent<scene::MaterialComponent>(std::move(mc));

        CreateBoneHierarchyForModel(*ctx.activeScene, root, skinnedRenderers, *model);
        return root.GetID();
    }

    // 静的モデルは MeshRenderer が 1 メッシュしか持てないため、従来どおり
    // submesh ごとに子 GameObject を作る。
    auto addStaticRenderer = [&](scene::GameObject& target, int meshIndex) {
        renderer::Mesh* mesh = model->meshes[static_cast<size_t>(meshIndex)].get();
        scene::MeshRenderer mr;
        mr.mesh     = mesh;
        mr.meshPath = modelPath + ":" + std::to_string(meshIndex);
        target.AddComponent<scene::MeshRenderer>(std::move(mr));

        scene::MaterialComponent mc;
        mc.materialPath = ResolveImportedMaterialPath(modelPath, modelAsset, meshIndex, false);
        target.AddComponent<scene::MaterialComponent>(std::move(mc));
    };

    if (meshCount == 1) {
        addStaticRenderer(root, 0);
        return root.GetID();
    }

    for (int meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        auto& child = ctx.activeScene->CreateGameObject(
            meshNames[static_cast<size_t>(meshIndex)]);
        child.SetParent(root);
        addStaticRenderer(child, meshIndex);
    }
    return root.GetID();
}

std::string ResolveOrImportFbxModel(const std::string& fbxAssetPath)
{
    namespace fs = std::filesystem;
    const fs::path fbxLogical = util::FileSystem::PathFromUtf8(fbxAssetPath);
    const std::string stem = util::FileSystem::PathToUtf8(fbxLogical.stem());

    // インポート済みなら FBX 自身を返す。LoadModel / Load<ModelAsset> が Library コンテナへ解決する。
    if (asset::AssetManager::Load<asset::ModelAsset>(fbxAssetPath).IsValid())
        return fbxAssetPath;

    // 未インポート: デフォルト設定でその場インポートする (Unity のドロップと同じ体験)。
    const std::string fbxAbs = asset::AssetManager::ResolveAssetPath(fbxAssetPath);
    if (!util::FileSystem::Exists(fbxAbs)) {
        FBZZ_LOG_WARN("ResolveOrImportFbxModel: fbx not found [%s]", fbxAssetPath.c_str());
        return {};
    }
    const std::string outputDir = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(fbxAbs).parent_path() / stem);
    FbxImportOptions options{};
    FbxMetaSerializer::LoadOptions(fbxAbs, options);
    FbxMetaSerializer::SaveOptions(fbxAbs, options);
    FBZZ_LOG_INFO("ResolveOrImportFbxModel: auto-importing [%s]", fbxAssetPath.c_str());
    if (!FbxImportTool::Import(fbxAbs, outputDir, fbxAbs, options)) {
        FBZZ_LOG_ERROR("ResolveOrImportFbxModel: import failed [%s]", fbxAssetPath.c_str());
        return {};
    }
    FbxMetaSerializer::SaveCacheInfo(fbxAbs, options);

    // 過去のロード失敗が Null キャッシュされていると新規 fzasset が引けないため掃除する。
    asset::AssetManager::FlushFailed();
    if (asset::AssetManager::Load<asset::ModelAsset>(fbxAssetPath).IsValid())
        return fbxAssetPath;
    return {};
}

} // namespace fbzz::editor
