// FBZZ Engine
// ModelImporterUtils.cpp | fbzz::asset
// Assimp 型変換および静的・スキンメッシュ両パスで共有するメッシュ変換の実装。
#include "ModelImporterInternal.hpp"
#include <Engine/Asset/Skeleton.hpp>
#include <algorithm>

namespace fbzz::asset {

namespace {

// 再帰でバインド TRS を積み上げ、ボーンごとのリファレンス行列を求める。
void BuildReferencePoseRecursive(Skeleton& skeleton,
                                 int nodeIndex,
                                 const math::Matrix4& parentGlobal)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(skeleton.nodes.size())) return;
    const SkeletonNode& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
    const math::Matrix4 global = parentGlobal * math::Matrix4::TRS(
        node.bindTranslation, node.bindRotation, node.bindScale);

    if (node.boneIndex >= 0 &&
        node.boneIndex < static_cast<int>(skeleton.referencePose.size())) {
        const Bone& bone = skeleton.bones[static_cast<size_t>(node.boneIndex)];
        skeleton.referencePose[static_cast<size_t>(node.boneIndex)] =
            skeleton.rootInverseTransform * global * bone.offsetMatrix;
    }

    for (int child : node.children)
        BuildReferencePoseRecursive(skeleton, child, global);
}

} // namespace

void BuildReferencePose(Skeleton& skeleton)
{
    skeleton.referencePose.assign(skeleton.bones.size(), math::Matrix4::Identity());
    if (skeleton.rootNodeIndex < 0 || skeleton.nodes.empty()) return;
    BuildReferencePoseRecursive(skeleton, skeleton.rootNodeIndex,
                                math::Matrix4::Identity());
}

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
    // WHY: FBX の原点単位はセンチメートルが多い。UnitScaleFactor は「1 単位 = x cm」の値なので
    //      0.01 を掛けることでメートルに換算する。値がない場合も同じデフォルトを使う。
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

std::unique_ptr<renderer::Material> ImportMaterial(const aiScene* scene,
                                                    const aiMesh* mesh,
                                                    renderer::ResourceManager& resources)
{
    auto mat = std::make_unique<renderer::Material>();
    // paramData はシェーダー確定後に SyncMaterial が初期化するため、ここでは設定しない。
    // assimp の diffuse color は MaterialComponent 経由で設定する必要がある。
    (void)resources;
    return mat;
}

} // namespace fbzz::asset
