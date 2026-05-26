// FBZZ Engine
// ModelImporter.cpp | fbzz::asset
// Assimp を使った静的・スキンメッシュのインポート
// 外部モデル形式を Engine の Model / Mesh / Material / Skeleton へ変換する。
// 読み込み失敗は nullptr で返し、例外には頼らない。
#include <Engine/Asset/ModelImporter.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Core/Logger.hpp>
#include <Math/Matrix4.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::asset {

namespace {

constexpr unsigned int STATIC_ASSIMP_FLAGS =
    aiProcess_Triangulate
  | aiProcess_JoinIdenticalVertices
  | aiProcess_GenNormals
  | aiProcess_MakeLeftHanded
  | aiProcess_FlipWindingOrder
  | aiProcess_FlipUVs
  | aiProcess_PreTransformVertices;

constexpr unsigned int SKINNED_ASSIMP_FLAGS =
    aiProcess_Triangulate
  | aiProcess_GenNormals
  | aiProcess_MakeLeftHanded
  | aiProcess_FlipWindingOrder
  | aiProcess_FlipUVs;

struct VertexInfluences {
    std::array<uint32_t, 4> indices = {};
    std::array<float, 4> weights = {};

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

std::string ToString(const aiString& s)
{
    return std::string(s.C_Str());
}

std::string NormalizeName(const aiString& s)
{
    std::string result = ToString(s);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

math::Vector3 ToVector3(const aiVector3D& v, float unitScale)
{
    return { v.x * unitScale, v.y * unitScale, v.z * unitScale };
}

math::Quaternion ToQuaternion(const aiQuaternion& q)
{
    return { q.x, q.y, q.z, q.w };
}

math::Matrix4 ToMatrix4(const aiMatrix4x4& m, float unitScale)
{
    math::Matrix4 result;
    result.m[0][0] = m.a1; result.m[0][1] = m.a2; result.m[0][2] = m.a3; result.m[0][3] = m.a4 * unitScale;
    result.m[1][0] = m.b1; result.m[1][1] = m.b2; result.m[1][2] = m.b3; result.m[1][3] = m.b4 * unitScale;
    result.m[2][0] = m.c1; result.m[2][1] = m.c2; result.m[2][2] = m.c3; result.m[2][3] = m.c4 * unitScale;
    result.m[3][0] = m.d1; result.m[3][1] = m.d2; result.m[3][2] = m.d3; result.m[3][3] = m.d4;
    return result;
}

float ReadUnitScale(const aiScene* scene)
{
    float unitScale = 0.01f;
    if (!scene || !scene->mMetaData) return unitScale;

    double factorD = 1.0;
    float  factorF = 1.0f;
    if (scene->mMetaData->Get("UnitScaleFactor", factorD))
        unitScale = static_cast<float>(factorD) * 0.01f;
    else if (scene->mMetaData->Get("UnitScaleFactor", factorF))
        unitScale = factorF * 0.01f;
    return unitScale;
}

bool HasSkinning(const aiScene* scene)
{
    if (!scene) return false;
    if (scene->HasAnimations()) return true;
    for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
        if (scene->mMeshes[i]->HasBones()) return true;
    }
    return false;
}

std::shared_ptr<renderer::Material> ImportMaterial(const aiScene* scene,
                                                   const aiMesh* mesh,
                                                   renderer::ResourceManager& resources)
{
    auto mat = std::make_shared<renderer::Material>();
    if (mesh->mMaterialIndex < scene->mNumMaterials) {
        aiColor4D color(1.0f, 1.0f, 1.0f, 1.0f);
        scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        mat->params.albedo = { color.r, color.g, color.b, color.a };
    }
    mat->Init(resources);
    return mat;
}

std::vector<uint32_t> ImportIndices(const aiMesh* mesh)
{
    std::vector<uint32_t> indices;
    indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
    for (uint32_t fi = 0; fi < mesh->mNumFaces; ++fi) {
        const aiFace& f = mesh->mFaces[fi];
        if (f.mNumIndices != 3) continue;
        indices.push_back(f.mIndices[0]);
        indices.push_back(f.mIndices[1]);
        indices.push_back(f.mIndices[2]);
    }
    return indices;
}

renderer::Vertex ImportVertex(const aiMesh* mesh, uint32_t i, float unitScale)
{
    renderer::Vertex v{};
    v.position = ToVector3(mesh->mVertices[i], unitScale);
    v.normal = mesh->mNormals
        ? math::Vector3{ mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z }
        : math::Vector3{ 0.0f, 1.0f, 0.0f };
    v.tangent = mesh->mTangents
        ? math::Vector3{ mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z }
        : math::Vector3{ 1.0f, 0.0f, 0.0f };
    if (mesh->mTextureCoords[0])
        v.uv = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
    return v;
}

void ImportNodesRecursive(const aiNode* node,
                          int parentIndex,
                          float unitScale,
                          Skeleton& skeleton)
{
    const int nodeIndex = static_cast<int>(skeleton.nodes.size());
    SkeletonNode outNode;
    outNode.name = NormalizeName(node->mName);
    outNode.parentIndex = parentIndex;
    outNode.localBindTransform = ToMatrix4(node->mTransformation, unitScale);
    aiVector3D scaling;
    aiVector3D position;
    aiQuaternion rotation;
    node->mTransformation.Decompose(scaling, rotation, position);
    outNode.bindTranslation = ToVector3(position, unitScale);
    outNode.bindRotation = ToQuaternion(rotation);
    outNode.bindScale = { scaling.x, scaling.y, scaling.z };

    skeleton.nodeMap[outNode.name] = nodeIndex;
    skeleton.nodes.push_back(std::move(outNode));
    if (parentIndex >= 0)
        skeleton.nodes[static_cast<size_t>(parentIndex)].children.push_back(nodeIndex);
    else
        skeleton.rootNodeIndex = nodeIndex;

    for (uint32_t i = 0; i < node->mNumChildren; ++i)
        ImportNodesRecursive(node->mChildren[i], nodeIndex, unitScale, skeleton);
}

int EnsureBone(const aiBone* aiBonePtr,
               float unitScale,
               Skeleton& skeleton)
{
    const std::string name = NormalizeName(aiBonePtr->mName);
    auto it = skeleton.boneMap.find(name);
    if (it != skeleton.boneMap.end()) return it->second;

    if (static_cast<int>(skeleton.bones.size()) >= MAX_SKINNING_BONES) {
        FBZZ_LOG_WARN("ModelImporter: bone limit exceeded; ignoring bone '%s'", name.c_str());
        return -1;
    }

    int nodeIndex = -1;
    auto nodeIt = skeleton.nodeMap.find(name);
    if (nodeIt != skeleton.nodeMap.end()) {
        nodeIndex = nodeIt->second;
    } else {
        nodeIndex = static_cast<int>(skeleton.nodes.size());
        SkeletonNode node{};
        node.name = name;
        node.parentIndex = skeleton.rootNodeIndex;
        node.bindTranslation = math::Vector3::ZERO;
        node.bindRotation = math::Quaternion::Identity();
        node.bindScale = math::Vector3::ONE;
        skeleton.nodes.push_back(node);
        skeleton.nodeMap[name] = nodeIndex;
        if (skeleton.rootNodeIndex >= 0)
            skeleton.nodes[static_cast<size_t>(skeleton.rootNodeIndex)].children.push_back(nodeIndex);
    }

    Bone bone{};
    bone.name = name;
    bone.nodeIndex = nodeIndex;
    bone.offsetMatrix = ToMatrix4(aiBonePtr->mOffsetMatrix, unitScale);

    const int boneIndex = static_cast<int>(skeleton.bones.size());
    skeleton.bones.push_back(bone);
    skeleton.boneMap[name] = boneIndex;
    skeleton.nodes[static_cast<size_t>(nodeIndex)].boneIndex = boneIndex;
    return boneIndex;
}

void ImportAnimations(const aiScene* scene, float unitScale, Model& model)
{
    for (uint32_t ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* src = scene->mAnimations[ai];
        AnimationClip clip{};
        clip.name = NormalizeName(src->mName);
        if (clip.name.empty())
            clip.name = "Clip" + std::to_string(ai);
        clip.durationTicks = src->mDuration;
        clip.ticksPerSecond = src->mTicksPerSecond > 0.0 ? src->mTicksPerSecond : 30.0;

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

std::shared_ptr<Model> ImportStaticModel(const aiScene* scene,
                                         float unitScale,
                                         renderer::ResourceManager& resources)
{
    auto model = std::make_shared<Model>();
    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* src = scene->mMeshes[mi];

        std::vector<renderer::Vertex> vertices(src->mNumVertices);
        for (uint32_t i = 0; i < src->mNumVertices; ++i)
            vertices[i] = ImportVertex(src, i, unitScale);

        auto indices = ImportIndices(src);

        auto mesh = std::make_shared<renderer::Mesh>();
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        mesh->indexBuffer = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        mesh->vertexCount = static_cast<uint32_t>(vertices.size());
        mesh->indexCount = static_cast<uint32_t>(indices.size());
        mesh->cpuVertices = vertices;
        mesh->cpuIndices = indices;

        model->meshes.push_back(mesh);
        model->materials.push_back(ImportMaterial(scene, src, resources));
    }
    return model;
}

std::shared_ptr<Model> ImportSkinnedModel(const aiScene* scene,
                                          float unitScale,
                                          renderer::ResourceManager& resources)
{
    auto model = std::make_shared<Model>();
    model->skeleton = std::make_shared<Skeleton>();
    ImportNodesRecursive(scene->mRootNode, -1, unitScale, *model->skeleton);
    model->skeleton->rootInverseTransform =
        math::Matrix4::Inverse(ToMatrix4(scene->mRootNode->mTransformation, unitScale));

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* src = scene->mMeshes[mi];

        std::vector<renderer::SkinnedVertex> vertices(src->mNumVertices);
        std::vector<renderer::Vertex> cpuStaticVertices(src->mNumVertices);
        for (uint32_t i = 0; i < src->mNumVertices; ++i) {
            cpuStaticVertices[i] = ImportVertex(src, i, unitScale);
            vertices[i].position = cpuStaticVertices[i].position;
            vertices[i].normal = cpuStaticVertices[i].normal;
            vertices[i].tangent = cpuStaticVertices[i].tangent;
            vertices[i].uv = cpuStaticVertices[i].uv;
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

        auto mesh = std::make_shared<renderer::Mesh>();
        mesh->isSkinned = true;
        mesh->vertexBuffer = resources.CreateVertexBuffer(
            vertices.data(), vertices.size() * sizeof(renderer::SkinnedVertex), sizeof(renderer::SkinnedVertex));
        mesh->indexBuffer = resources.CreateIndexBuffer(indices.data(), static_cast<uint32_t>(indices.size()));
        mesh->vertexCount = static_cast<uint32_t>(vertices.size());
        mesh->indexCount = static_cast<uint32_t>(indices.size());
        mesh->cpuSkinnedVertices = vertices;
        mesh->cpuVertices = cpuStaticVertices;
        mesh->cpuIndices = indices;

        model->meshes.push_back(mesh);
        model->materials.push_back(ImportMaterial(scene, src, resources));
    }

    ImportAnimations(scene, unitScale, *model);
    return model;
}

} // anonymous namespace

std::shared_ptr<Model> ModelImporter::Import(
    const std::string& path,
    renderer::ResourceManager& resources)
{
    Assimp::Importer probeImporter;
    const aiScene* probeScene = probeImporter.ReadFile(path, SKINNED_ASSIMP_FLAGS);
    if (!probeScene) {
        FBZZ_LOG_ERROR("ModelImporter: failed to load %s — %s",
                       path.c_str(), probeImporter.GetErrorString());
        return nullptr;
    }
    if (probeScene->mNumMeshes == 0 && probeScene->mNumAnimations == 0) {
        FBZZ_LOG_ERROR("ModelImporter: %s has no meshes and no animations", path.c_str());
        return nullptr;
    }

    const bool skinned = HasSkinning(probeScene);
    const float probeUnitScale = ReadUnitScale(probeScene);
    FBZZ_LOG_INFO("ModelImporter: %s unitScale=%.4f skinned=%d",
                  path.c_str(), probeUnitScale, skinned ? 1 : 0);

    if (skinned)
        return ImportSkinnedModel(probeScene, probeUnitScale, resources);

    // 静的メッシュとして、頂点結合フラグ付きで再インポートする
    Assimp::Importer staticImporter;
    const aiScene* staticScene = staticImporter.ReadFile(path, STATIC_ASSIMP_FLAGS);
    if (!staticScene || staticScene->mNumMeshes == 0) {
        FBZZ_LOG_ERROR("ModelImporter: failed to load static model %s", path.c_str());
        return nullptr;
    }

    return ImportStaticModel(staticScene, ReadUnitScale(staticScene), resources);

}

} // namespace fbzz::asset
