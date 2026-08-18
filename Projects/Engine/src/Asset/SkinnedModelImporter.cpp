// FBZZ Engine
// SkinnedModelImporter.cpp | fbzz::asset
// スキンメッシュ・スケルトン・アニメーションクリップのインポートパイプライン。
// WHY: aiProcess_PreTransformVertices を使わずボーン階層を保持する。
//      静的メッシュと異なり頂点ごとのボーンインデックス・ウェイトを CPU 側で構築し、
//      GPU スキニングに必要な SkinnedVertex レイアウトへ変換する。
#include "ModelImporterInternal.hpp"
#include <Engine/Core/Logger.hpp>
#include <array>
#include <string>
#include <unordered_map>

namespace fbzz::asset {

namespace {

// 1 頂点に影響するボーンのインデックス・ウェイトを最大 4 本管理する。
// WHY: GPU スキニングシェーダーの定数バッファは 4 ボーン固定のため上位 4 本に絞る。
// WHAT: 挿入時に降順ソートを維持し、常に影響度の大きいボーンを残す。
struct VertexInfluences {
    std::array<uint32_t, 4> indices = {};
    std::array<float, 4>    weights = {};

    // weight が既存スロットの最小ウェイトより大きければ差し替えて降順を維持する。
    void Add(uint32_t boneIndex, float weight)
    {
        if (weight <= 0.0f) return;
        for (int i = 0; i < 4; ++i) {
            if (weight > weights[static_cast<size_t>(i)]) {
                for (int j = 3; j > i; --j) {
                    weights[static_cast<size_t>(j)] = weights[static_cast<size_t>(j - 1)];
                    indices[static_cast<size_t>(j)] = indices[static_cast<size_t>(j - 1)];
                }
                weights[static_cast<size_t>(i)] = weight;
                indices[static_cast<size_t>(i)] = boneIndex;
                return;
            }
        }
    }

    // 4 本の総和が 1 になるよう正規化する。全 0 の場合はボーン 0 に 1.0 を割り当てる。
    void Normalize()
    {
        float sum = 0.0f;
        for (float w : weights) sum += w;
        if (sum <= 0.0f) {
            weights[0] = 1.0f;
            indices[0] = 0;
            return;
        }
        for (float& w : weights) w /= sum;
    }
};

// aiNode 木を DFS で走査し、SkeletonNode を skeleton.nodes へ追加する。
// WHY: スケルトンは「ノード木全体」と「ボーンのサブセット」を分離して管理する。
//      ボーンが存在しないノードも保持することで、親子関係とバインドポーズを維持できる。
void ImportNodesRecursive(const aiNode* node,
                          int parentIndex,
                          float unitScale,
                          Skeleton& skeleton)
{
    const int nodeIndex = static_cast<int>(skeleton.nodes.size());
    SkeletonNode outNode;
    outNode.name              = NormalizeName(node->mName);
    outNode.parentIndex       = parentIndex;
    outNode.localBindTransform = ToMatrix4(node->mTransformation, unitScale);

    aiVector3D   scaling;
    aiVector3D   position;
    aiQuaternion rotation;
    node->mTransformation.Decompose(scaling, rotation, position);
    outNode.bindTranslation = ToVector3(position, unitScale);
    outNode.bindRotation    = ToQuaternion(rotation);
    outNode.bindScale       = { scaling.x, scaling.y, scaling.z };

    skeleton.nodeMap[outNode.name] = nodeIndex;
    skeleton.nodes.push_back(std::move(outNode));
    if (parentIndex >= 0)
        skeleton.nodes[static_cast<size_t>(parentIndex)].children.push_back(nodeIndex);
    else
        skeleton.rootNodeIndex = nodeIndex;

    for (uint32_t i = 0; i < node->mNumChildren; ++i)
        ImportNodesRecursive(node->mChildren[i], nodeIndex, unitScale, skeleton);
}

// aiBone を Skeleton::bones に登録しボーンインデックスを返す。
// WHY: Assimp は同一ボーン名が複数メッシュに現れるため、名前で重複チェックする。
//      ノード木に存在しない補助ボーンはルートの子として動的に追加する。
int EnsureBone(const aiBone* aiBonePtr,
               float unitScale,
               Skeleton& skeleton)
{
    const std::string name = NormalizeName(aiBonePtr->mName);
    auto it = skeleton.boneMap.find(name);
    if (it != skeleton.boneMap.end()) return it->second;

    if (static_cast<int>(skeleton.bones.size()) >= MAX_SKINNING_BONES) {
        FBZZ_LOG_WARN("SkinnedModelImporter: bone limit exceeded; ignoring bone '%s'", name.c_str());
        return -1;
    }

    int nodeIndex = -1;
    auto nodeIt = skeleton.nodeMap.find(name);
    if (nodeIt != skeleton.nodeMap.end()) {
        nodeIndex = nodeIt->second;
    } else {
        nodeIndex = static_cast<int>(skeleton.nodes.size());
        SkeletonNode node{};
        node.name            = name;
        node.parentIndex     = skeleton.rootNodeIndex;
        node.bindTranslation = math::Vector3::ZERO;
        node.bindRotation    = math::Quaternion::Identity();
        node.bindScale       = math::Vector3::ONE;
        skeleton.nodes.push_back(node);
        skeleton.nodeMap[name] = nodeIndex;
        if (skeleton.rootNodeIndex >= 0)
            skeleton.nodes[static_cast<size_t>(skeleton.rootNodeIndex)].children.push_back(nodeIndex);
    }

    Bone bone{};
    bone.name         = name;
    bone.nodeIndex    = nodeIndex;
    bone.offsetMatrix = ToMatrix4(aiBonePtr->mOffsetMatrix, unitScale);

    const int boneIndex = static_cast<int>(skeleton.bones.size());
    skeleton.bones.push_back(bone);
    skeleton.boneMap[name] = boneIndex;
    skeleton.nodes[static_cast<size_t>(nodeIndex)].boneIndex = boneIndex;
    return boneIndex;
}

// aiAnimation を AnimationClip へ変換してモデルに追加する。
// WHAT: Position / Rotation / Scale の各キーを NodeAnimationTrack に格納し、
//       AnimatorSystem が補間再生できる形式にする。
void ImportAnimations(const aiScene* scene, float unitScale, Model& model)
{
    for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* src = scene->mAnimations[ai];
        AnimationClip clip{};
        clip.name = NormalizeName(src->mName);
        if (clip.name.empty())
            clip.name = "Clip" + std::to_string(ai);
        clip.durationTicks  = src->mDuration;
        clip.ticksPerSecond = src->mTicksPerSecond > 0.0 ? src->mTicksPerSecond : 30.0;
        clip.durationSeconds = clip.ticksPerSecond > 0.0
            ? clip.durationTicks / clip.ticksPerSecond
            : 0.0;

        for (uint32_t ci = 0; ci < src->mNumChannels; ++ci) {
            const aiNodeAnim* channel = src->mChannels[ci];
            NodeAnimationTrack track{};
            track.nodeName = NormalizeName(channel->mNodeName);

            track.positions.reserve(channel->mNumPositionKeys);
            for (uint32_t i = 0; i < channel->mNumPositionKeys; ++i) {
                track.positions.push_back({
                    channel->mPositionKeys[i].mTime,
                    ToVector3(channel->mPositionKeys[i].mValue, unitScale)
                });
            }

            track.rotations.reserve(channel->mNumRotationKeys);
            for (uint32_t i = 0; i < channel->mNumRotationKeys; ++i) {
                track.rotations.push_back({
                    channel->mRotationKeys[i].mTime,
                    ToQuaternion(channel->mRotationKeys[i].mValue)
                });
            }

            track.scales.reserve(channel->mNumScalingKeys);
            for (uint32_t i = 0; i < channel->mNumScalingKeys; ++i) {
                track.scales.push_back({
                    channel->mScalingKeys[i].mTime,
                    { channel->mScalingKeys[i].mValue.x,
                      channel->mScalingKeys[i].mValue.y,
                      channel->mScalingKeys[i].mValue.z }
                });
            }

            clip.tracks.push_back(std::move(track));
        }

        model.clips.push_back(std::move(clip));
    }
}

} // anonymous namespace

std::unique_ptr<Model> ImportSkinnedModel(const aiScene* scene,
                                          float unitScale,
                                          renderer::ResourceManager& resources)
{
    auto model = std::make_unique<Model>();
    model->skeleton = std::make_unique<Skeleton>();
    ImportNodesRecursive(scene->mRootNode, -1, unitScale, *model->skeleton);
    model->skeleton->rootInverseTransform =
        math::Matrix4::Inverse(ToMatrix4(scene->mRootNode->mTransformation, unitScale));

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* src = scene->mMeshes[mi];

        std::vector<renderer::SkinnedVertex> vertices(src->mNumVertices);
        std::vector<renderer::Vertex>        cpuStaticVertices(src->mNumVertices);
        for (uint32_t i = 0; i < src->mNumVertices; ++i) {
            cpuStaticVertices[i]   = ImportVertex(src, i, unitScale);
            vertices[i].position  = cpuStaticVertices[i].position;
            vertices[i].normal    = cpuStaticVertices[i].normal;
            vertices[i].tangent   = cpuStaticVertices[i].tangent;
            vertices[i].uv        = cpuStaticVertices[i].uv;
        }

        std::vector<VertexInfluences> influences(src->mNumVertices);
        for (uint32_t bi = 0; bi < src->mNumBones; ++bi) {
            const aiBone* srcBone = src->mBones[bi];
            const int boneIndex = EnsureBone(srcBone, unitScale, *model->skeleton);
            if (boneIndex < 0) continue;
            for (uint32_t wi = 0; wi < srcBone->mNumWeights; ++wi) {
                const aiVertexWeight& w = srcBone->mWeights[wi];
                if (w.mVertexId < src->mNumVertices)
                    influences[w.mVertexId].Add(static_cast<uint32_t>(boneIndex), w.mWeight);
            }
        }

        for (uint32_t i = 0; i < src->mNumVertices; ++i) {
            influences[i].Normalize();
            for (int j = 0; j < 4; ++j) {
                vertices[i].boneIndices[j] = influences[i].indices[static_cast<size_t>(j)];
                vertices[i].boneWeights[j] = influences[i].weights[static_cast<size_t>(j)];
            }
        }

        auto indices = ImportIndices(src);

        auto mesh = std::make_unique<renderer::Mesh>();
        mesh->isSkinned    = true;
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            vertices.data(), vertices.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
        mesh->indexBuffer  = resources.CreateIndexBuffer(
            indices.data(), static_cast<uint32_t>(indices.size()));
        mesh->vertexCount        = static_cast<uint32_t>(vertices.size());
        mesh->indexCount         = static_cast<uint32_t>(indices.size());
        mesh->cpuSkinnedVertices = vertices;
        mesh->cpuVertices        = cpuStaticVertices;
        mesh->cpuIndices         = indices;
        mesh->ComputeBounds();

        model->meshes.push_back(std::move(mesh));
        model->materials.push_back(ImportMaterial(scene, src, resources));
    }

    // どのノードがどの submesh を描くかの対応表。
    // WHY スケルトンと別に持つか: Skeleton::nodes は「変形の材料」で、ボーンも補助ノードも
    //     含む全ノードが並ぶ。こちらが答えるのは「GameObject をどこに何個作り、それぞれ
    //     どの submesh を描かせるか」という配置の問だけで、目的が違う。
    //     スキンドは PreTransformVertices を通していないため、階層はそのまま残っている。
    ImportModelNodes(scene, unitScale, *model);

    // 無アニメ時の既定パレット。単位行列を使わないための前提データ。
    if (model->skeleton) BuildReferencePose(*model->skeleton);

    ImportAnimations(scene, unitScale, *model);
    return model;
}

} // namespace fbzz::asset
