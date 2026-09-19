/// @file    ModelPlacement.cpp
/// @brief   .fbx アセットから Scene 用 GameObject 階層を構築する。
/// @author  Hasegawa Jin
/// @date    2026-06-18
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

} // namespace

std::string FindImportedMaterialPath(const std::string& modelPath,
                                     const asset::ModelAsset* modelAsset,
                                     int meshIndex)
{
    if (!modelAsset || modelAsset->lods.empty() || meshIndex < 0)
        return {};

    const auto& submeshes = modelAsset->lods[0].submeshes;
    if (meshIndex >= static_cast<int>(submeshes.size()))
        return {};

    const uint32_t slotIndex = submeshes[static_cast<size_t>(meshIndex)].materialSlotIndex;
    if (slotIndex >= modelAsset->materialSlotNames.size())
        return {};

    const bool isFbx =
        util::StringUtils::ToLower(util::FileSystem::GetExtension(modelPath)) == ".fbx";
    const std::filesystem::path modelFilePath =
        util::FileSystem::PathFromUtf8(modelPath);
    const std::filesystem::path modelDir = isFbx
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
    /// @note Scene には原本 FBX から導出した論理パスを保存し、.mat の実体は AssetManager が
    ///       Assets 側または Library/Baked 側から解決する。
    const std::filesystem::path matFsPath =
        modelDir / "materials" / (fileStem + ".mat");
    const std::string normalized = util::FileSystem::NormalizePathSeparators(
        util::FileSystem::PathToUtf8(matFsPath));
    const std::string lower = util::StringUtils::ToLower(normalized);
    std::string logicalPath = normalized;
    const size_t assetsPos = lower.rfind("/assets/");
    if (assetsPos != std::string::npos)
        logicalPath = normalized.substr(assetsPos + 1);
    else if (lower.starts_with("assets/"))
        logicalPath = normalized;

    if (isFbx) {
        /// @note FBX はシーンに保存する論理パスであり、実体の .mat は Library/Baked にある。Assets 側の物理パスだけを確認すると現在の隔離配置を見失って常に Fallback.mat へ落ち、D&D 直後のモデルが元材質を失う。
        if (!util::FileSystem::Exists(asset::AssetManager::ResolveAssetPath(logicalPath)))
            return {};
        return logicalPath;
    }

    if (!util::FileSystem::Exists(matFsPath))
        return {};

    if (logicalPath != normalized)
        return logicalPath;
    return normalized;
}

namespace {

std::string ResolveImportedMaterialPath(const std::string& modelPath,
                                        const asset::ModelAsset* modelAsset,
                                        int meshIndex,
                                        bool skinned)
{
    std::string path = FindImportedMaterialPath(modelPath, modelAsset, meshIndex);
    if (!path.empty()) return path;
    return skinned ? "Assets/Materials/Fallback/FallbackSkinned.mat"
                   : "Assets/Materials/Fallback/Fallback.mat";
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

    /// @note v2 以前の .fzasset は名前を持たないため、FBX 原本を読んで互換的に補完する。
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

/// boneParent はボーン階層をぶら下げる GameObject。owner は BoneComponent が指す
/// 「このスケルトンを使う Renderer の代表」。
/// @note 2 つに分ける: ノードごとに子 GameObject へ分けた構成では Renderer は Body / Visor といった子に付き、ボーンはモデルのルート直下 (Unity の Armature と同じ位置) へ並べたい。両者を同じ引数で兼ねるとボーンが Body の下に潜り階層が DCC と一致しなくなる。
void CreateBoneHierarchyForModel(scene::Scene& scene,
                                 scene::GameObject& boneParent,
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
            scene, owner, boneParent, renderers, skeleton, skeleton.rootNodeIndex, createdNodes);
        for (auto* smr : renderers)
            if (smr) smr->skeletonRootEntity = rootEntity;
    }

    /// @note 一部 DCC は rootNodeIndex から到達できない補助ノードを含むため、未生成ノードも parentIndex を見て階層へ接続する。
    for (int nodeIndex = 0; nodeIndex < static_cast<int>(skeleton.nodes.size()); ++nodeIndex) {
        if (createdNodes[static_cast<size_t>(nodeIndex)] != scene::EntityID::INVALID)
            continue;
        const int parentIndex = skeleton.nodes[static_cast<size_t>(nodeIndex)].parentIndex;
        scene::GameObject* parent = &boneParent;
        if (parentIndex >= 0) {
            const scene::EntityID parentEntity = CreateBoneHierarchyNode(
                scene, owner, boneParent, renderers, skeleton, parentIndex, createdNodes);
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

    auto* model = asset::AssetManager::LoadAndGet<asset::Model>(modelPath);
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

    const bool anySkinned = [&] {
        for (const auto& mesh : model->meshes)
            if (mesh && mesh->isSkinned) return true;
        return false;
    }();

    if (anySkinned) {
        /// @note DCC のノード 1 個 = 1 GameObject (Unity と同じ分割単位)。ノード内のマテリアル分割は、その Renderer の submesh 列 = マテリアルスロット列。
        /// @note meshes を平坦に 1 個ずつ子へ配らない: Assimp は 1 つの DCC メッシュをマテリアルごとに分割するため、Body に 2 材質が載っているだけで Body が 2 つの GameObject に割れる。ノードで束ねることで階層が DCC のアウトライナと一致し、名前から部位が読める状態を保つ。
        std::vector<const asset::ModelNode*> meshNodes;
        for (const auto& node : model->nodes)
            if (node.HasMeshes()) meshNodes.push_back(&node);

        /// @note Renderer 1 個ぶんを組み立てる。submeshIndices が空なら「モデル全体」。
        /// @note MaterialComponent を必ず付ける: 空だと GeometryPass が描画をスキップし、D&D した結果がユーザーに見えない状態になるため、既定材を必ず割り当てる。
        auto addSkinnedPart = [&](scene::GameObject& target,
                                  const std::vector<uint32_t>& submeshIndices) {
            scene::SkinnedMeshRenderer smr;
            smr.modelPath      = modelPath;
            smr.model          = model;
            smr.submeshIndices = submeshIndices;
            skinnedRenderers.push_back(
                &target.AddComponent<scene::SkinnedMeshRenderer>(std::move(smr)));

            /// @note スロット数は「この Renderer が描く submesh の数」。
            const size_t slotCount = submeshIndices.empty()
                ? static_cast<size_t>(meshCount) : submeshIndices.size();
            scene::MaterialComponent mc;
            mc.ResizeSlots(slotCount);
            for (size_t slot = 0; slot < slotCount; ++slot) {
                const int meshIndex = submeshIndices.empty()
                    ? static_cast<int>(slot)
                    : static_cast<int>(submeshIndices[slot]);
                if (meshIndex < 0 || meshIndex >= meshCount) continue;
                const renderer::Mesh* mesh = model->meshes[static_cast<size_t>(meshIndex)].get();
                mc.RawSlotAt(slot).materialPath =
                    ResolveImportedMaterialPath(modelPath, modelAsset, meshIndex,
                                                mesh && mesh->isSkinned);
            }
            target.AddComponent<scene::MaterialComponent>(std::move(mc));
        };

        if (meshNodes.size() <= 1) {
            /// @note ノードが 1 個 (or ノード情報が無い v3 以前のベイク) なら、分ける意味が無い。
            ///       root 自身が描画担当になり、従来と同じ 1 GameObject 構成になる。
            addSkinnedPart(root, meshNodes.size() == 1
                ? meshNodes[0]->meshIndices : std::vector<uint32_t>{});
            if (meshNodes.size() == 1 && !meshNodes[0]->name.empty())
                root.name = meshNodes[0]->name;
        } else {
            for (const asset::ModelNode* node : meshNodes) {
                auto& child = ctx.activeScene->CreateGameObject(
                    node->name.empty() ? std::string("Mesh") : node->name);
                child.layer = root.layer;
                child.SetParent(root);
                /// @note ノードの TRS は入れない: スキンド頂点はボーンパレットで変形されるためメッシュノードの変換は描画に使われず、Transform へ入れると二重に掛かる。詳細は `FzModelFormat.hpp` の `FZMODEL_FLAG_NODE_TRANSFORMS_BAKED`。
                addSkinnedPart(child, node->meshIndices);
            }
        }

        /// @note ボーンは root 直下へ (Unity の Armature と同じ位置)。
        ///       owner は代表 Renderer — BoneComponent::skinnedMeshEntity がこれを指し、
        ///       AnimatorSystem が「どのスケルトンか」を辿る足がかりになる。
        scene::GameObject* owner = &root;
        if (!skinnedRenderers.empty() && meshNodes.size() > 1) {
            /// @note 代表は最初の Renderer が付いた子。
            for (int i = 0; i < root.GetChildCount(); ++i) {
                scene::GameObject* child = root.GetChild(i);
                if (child && child->GetComponent<scene::SkinnedMeshRenderer>()) {
                    owner = child;
                    break;
                }
            }
        }
        CreateBoneHierarchyForModel(*ctx.activeScene, root, *owner, skinnedRenderers, *model);
        return root.GetID();
    }

    /// @note 静的モデルは MeshRenderer が 1 メッシュしか持てないため、従来どおり
    ///       submesh ごとに子 GameObject を作る。
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

    /// @note インポート済みなら FBX 自身を返す。`Load<Model>` / `Load<ModelAsset>` が Library コンテナへ解決する。
    if (asset::AssetManager::Load<asset::ModelAsset>(fbxAssetPath).IsValid())
        return fbxAssetPath;

    /// @note 未インポート: デフォルト設定でその場インポートする (Unity のドロップと同じ体験)。
    const std::string fbxAbs = asset::AssetManager::ResolveAssetPath(fbxAssetPath);
    if (!util::FileSystem::Exists(fbxAbs)) {
        FBZZ_LOG_WARN("ResolveOrImportFbxModel: fbx not found [%s]", fbxAssetPath.c_str());
        return {};
    }
    const std::string outputDir = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(fbxAbs).parent_path() / stem);
    FbxImportOptions options{};
    /// @note .meta を持たない新規 FBX は既定オプションでインポートするため、読み込み失敗は正常系。
    (void)FbxMetaSerializer::LoadOptions(fbxAbs, options);
    FBZZ_LOG_INFO("ResolveOrImportFbxModel: auto-importing [%s]", fbxAssetPath.c_str());
    if (!FbxImportTool::Import(fbxAbs, outputDir, fbxAbs, options)) {
        FBZZ_LOG_ERROR("ResolveOrImportFbxModel: import failed [%s]", fbxAssetPath.c_str());
        return {};
    }
    /// @note 片方が失敗しても他方は書き切る。fingerprint だけ古いと次回の再インポート判定が狂うため。
    const bool optionsSaved = FbxMetaSerializer::SaveOptions(fbxAbs, options);
    const bool cacheSaved   = FbxMetaSerializer::SaveCacheInfo(fbxAbs, options);
    if (!optionsSaved || !cacheSaved) {
        FBZZ_LOG_WARN("ResolveOrImportFbxModel: .meta の更新に失敗 [%s]", fbxAssetPath.c_str());
    }

    /// @note 過去のロード失敗が Null キャッシュされていると新規 fzasset が引けないため掃除する。
    asset::AssetManager::FlushFailed();
    if (asset::AssetManager::Load<asset::ModelAsset>(fbxAssetPath).IsValid())
        return fbxAssetPath;
    return {};
}

} // namespace fbzz::editor
