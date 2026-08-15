// FBZZ Engine
// ModelSubExporter.cpp | fbzz::editor
// FBX → .fzasset (FZMD) + 代表 .mesh バイナリを生成する。
// .fzasset はパッケージ展開用、.mesh は Detail / Foliage / MeshRenderer から直接参照する代表メッシュ。
#include <Editor/Import/ModelSubExporter.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Asset/FzModelFormat.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/mesh.h>
#include <assimp/scene.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

namespace {

// ── 頂点レイアウト (renderer と同じビット列) ──────────────────────────────
struct Vertex {
    float position[3];
    float normal[3];
    float tangent[3];
    float uv[2];
};
static_assert(sizeof(Vertex) == 44, "Vertex size mismatch with renderer::Vertex");

struct SkinnedVertex {
    float    position[3];
    float    normal[3];
    float    tangent[3];
    float    uv[2];
    uint32_t boneIndices[4];
    float    boneWeights[4];
};
static_assert(sizeof(SkinnedVertex) == 76, "SkinnedVertex size mismatch");

Vertex ConvertVertex(const aiMesh* mesh, uint32_t i, float scale)
{
    Vertex v{};
    v.position[0] = mesh->mVertices[i].x * scale;
    v.position[1] = mesh->mVertices[i].y * scale;
    v.position[2] = mesh->mVertices[i].z * scale;
    if (mesh->mNormals) {
        v.normal[0] = mesh->mNormals[i].x;
        v.normal[1] = mesh->mNormals[i].y;
        v.normal[2] = mesh->mNormals[i].z;
    } else { v.normal[1] = 1.0f; }
    if (mesh->mTangents) {
        v.tangent[0] = mesh->mTangents[i].x;
        v.tangent[1] = mesh->mTangents[i].y;
        v.tangent[2] = mesh->mTangents[i].z;
    } else { v.tangent[0] = 1.0f; }
    if (mesh->mTextureCoords[0]) {
        v.uv[0] = mesh->mTextureCoords[0][i].x;
        v.uv[1] = mesh->mTextureCoords[0][i].y;
    }
    return v;
}

aiVector3D TransformPoint(const aiMatrix4x4& m, const aiVector3D& v)
{
    return {
        m.a1 * v.x + m.a2 * v.y + m.a3 * v.z + m.a4,
        m.b1 * v.x + m.b2 * v.y + m.b3 * v.z + m.b4,
        m.c1 * v.x + m.c2 * v.y + m.c3 * v.z + m.c4
    };
}

aiVector3D TransformDirection(const aiMatrix4x4& m, const aiVector3D& v)
{
    aiVector3D out{
        m.a1 * v.x + m.a2 * v.y + m.a3 * v.z,
        m.b1 * v.x + m.b2 * v.y + m.b3 * v.z,
        m.c1 * v.x + m.c2 * v.y + m.c3 * v.z
    };
    const float len = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
    if (len > 1e-6f) out *= 1.0f / len;
    return out;
}

Vertex ConvertVertexWithNodeTransform(const aiMesh* mesh, uint32_t i, float scale, const aiMatrix4x4& transform)
{
    Vertex v = ConvertVertex(mesh, i, 1.0f);
    const aiVector3D pos = TransformPoint(transform, mesh->mVertices[i]);
    v.position[0] = pos.x * scale;
    v.position[1] = pos.y * scale;
    v.position[2] = pos.z * scale;
    if (mesh->mNormals) {
        const aiVector3D n = TransformDirection(transform, mesh->mNormals[i]);
        v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
    }
    if (mesh->mTangents) {
        const aiVector3D t = TransformDirection(transform, mesh->mTangents[i]);
        v.tangent[0] = t.x; v.tangent[1] = t.y; v.tangent[2] = t.z;
    }
    return v;
}

Vertex ConvertVertexWithStaticAxisFix(const aiMesh* mesh,
                                      uint32_t i,
                                      float scale,
                                      const aiMatrix4x4& transform,
                                      const aiQuaternion& axisInvQ,
                                      float axisInvS)
{
    Vertex v = ConvertVertex(mesh, i, 1.0f);
    const aiVector3D nodePos = TransformPoint(transform, mesh->mVertices[i]);
    const aiVector3D pos = axisInvQ.Rotate(nodePos * axisInvS);
    v.position[0] = pos.x * scale;
    v.position[1] = pos.y * scale;
    v.position[2] = pos.z * scale;
    if (mesh->mNormals) {
        const aiVector3D nodeN = TransformDirection(transform, mesh->mNormals[i]);
        const aiVector3D n = axisInvQ.Rotate(nodeN);
        v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
    }
    if (mesh->mTangents) {
        const aiVector3D nodeT = TransformDirection(transform, mesh->mTangents[i]);
        const aiVector3D t = axisInvQ.Rotate(nodeT);
        v.tangent[0] = t.x; v.tangent[1] = t.y; v.tangent[2] = t.z;
    }
    return v;
}

void CollectMeshNodeTransforms(const aiNode* node,
                               const aiMatrix4x4& parent,
                               std::unordered_map<uint32_t, aiMatrix4x4>& outTransforms)
{
    if (!node) return;
    const aiMatrix4x4 global = parent * node->mTransformation;
    for (uint32_t i = 0; i < node->mNumMeshes; ++i)
        outTransforms.emplace(node->mMeshes[i], global);
    for (uint32_t i = 0; i < node->mNumChildren; ++i)
        CollectMeshNodeTransforms(node->mChildren[i], global, outTransforms);
}

struct Influence {
    std::array<uint32_t, 4> idx{};
    std::array<float, 4>    wgt{};
    void Add(uint32_t bi, float w) {
        if (w <= 0.0f) return;
        for (int i = 0; i < 4; ++i) {
            if (w > wgt[static_cast<size_t>(i)]) {
                for (int j = 3; j > i; --j) {
                    wgt[static_cast<size_t>(j)] = wgt[static_cast<size_t>(j-1)];
                    idx[static_cast<size_t>(j)] = idx[static_cast<size_t>(j-1)];
                }
                wgt[static_cast<size_t>(i)] = w;
                idx[static_cast<size_t>(i)] = bi;
                return;
            }
        }
    }
    void Normalize() {
        float sum = 0.0f;
        for (float f : wgt) sum += f;
        if (sum > 0.0f) for (float& f : wgt) f /= sum;
        else wgt[0] = 1.0f;
    }
};

void ComputeBoundsV(const float* positions, size_t count, size_t stride,
                    float center[3], float& radius)
{
    if (!count) { center[0]=center[1]=center[2]=0; radius=0; return; }
    float mn[3], mx[3];
    for (int k=0; k<3; ++k) mn[k] = mx[k] = positions[k];
    for (size_t i=1; i<count; ++i) {
        const float* p = positions + i * stride;
        for (int k=0; k<3; ++k) {
            if (p[k]<mn[k]) mn[k]=p[k];
            if (p[k]>mx[k]) mx[k]=p[k];
        }
    }
    for (int k=0; k<3; ++k) center[k]=(mn[k]+mx[k])*0.5f;
    radius = 0.0f;
    for (size_t i=0; i<count; ++i) {
        const float* p = positions + i * stride;
        float dx=p[0]-center[0], dy=p[1]-center[1], dz=p[2]-center[2];
        float r = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (r > radius) radius = r;
    }
}

aiVector3D TransformVector(const aiMatrix4x4& m, const aiVector3D& v)
{
    return {
        m.a1 * v.x + m.a2 * v.y + m.a3 * v.z,
        m.b1 * v.x + m.b2 * v.y + m.b3 * v.z,
        m.c1 * v.x + m.c2 * v.y + m.c3 * v.z
    };
}

bool WriteMorphTargets(std::ofstream& out,
                       const aiMesh& mesh,
                       float unitScale,
                       bool applyNodeTransform,
                       const aiMatrix4x4& transform,
                       bool applyAxisFix,
                       const aiQuaternion& axisInvQ,
                       float axisInvScale,
                       // スキンメッシュ頂点へ焼いたバインド回転。デルタにも同じ回転を
                       // 掛けないとモーフだけ元の Z-up 方向へずれる。静的メッシュは
                       // transform 側で既に回っているので identity を渡す。
                       const aiQuaternion& bakeQ)
{
    using namespace asset;
    for (uint32_t targetIndex = 0; targetIndex < mesh.mNumAnimMeshes; ++targetIndex) {
        const aiAnimMesh* target = mesh.mAnimMeshes[targetIndex];
        FzMorphTargetHeader header{};
        const std::string targetName = target->mName.length > 0
            ? target->mName.C_Str() : ("Morph_" + std::to_string(targetIndex));
        std::memcpy(header.name, targetName.data(),
                    std::min(targetName.size(), sizeof(header.name) - 1));
        header.vertexCount = mesh.mNumVertices;
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));

        std::vector<FzMorphDelta> deltas(mesh.mNumVertices);
        for (uint32_t i = 0; i < mesh.mNumVertices; ++i) {
            aiVector3D positionDelta = target->mVertices
                ? target->mVertices[i] - mesh.mVertices[i] : aiVector3D{};
            if (applyNodeTransform) positionDelta = TransformVector(transform, positionDelta);
            if (applyAxisFix) positionDelta = axisInvQ.Rotate(positionDelta * axisInvScale);
            positionDelta *= unitScale;
            positionDelta = bakeQ.Rotate(positionDelta);

            aiVector3D normalDelta{};
            if (target->mNormals && mesh.mNormals) {
                aiVector3D targetNormal = target->mNormals[i];
                aiVector3D baseNormal = mesh.mNormals[i];
                if (applyNodeTransform) {
                    targetNormal = TransformDirection(transform, targetNormal);
                    baseNormal = TransformDirection(transform, baseNormal);
                }
                if (applyAxisFix) {
                    targetNormal = axisInvQ.Rotate(targetNormal);
                    baseNormal = axisInvQ.Rotate(baseNormal);
                }
                normalDelta = bakeQ.Rotate(targetNormal - baseNormal);
            }

            aiVector3D tangentDelta{};
            if (target->mTangents && mesh.mTangents) {
                aiVector3D targetTangent = target->mTangents[i];
                aiVector3D baseTangent = mesh.mTangents[i];
                if (applyNodeTransform) {
                    targetTangent = TransformDirection(transform, targetTangent);
                    baseTangent = TransformDirection(transform, baseTangent);
                }
                if (applyAxisFix) {
                    targetTangent = axisInvQ.Rotate(targetTangent);
                    baseTangent = axisInvQ.Rotate(baseTangent);
                }
                tangentDelta = bakeQ.Rotate(targetTangent - baseTangent);
            }
            auto& delta = deltas[i];
            delta.position[0] = positionDelta.x;
            delta.position[1] = positionDelta.y;
            delta.position[2] = positionDelta.z;
            delta.normal[0] = normalDelta.x;
            delta.normal[1] = normalDelta.y;
            delta.normal[2] = normalDelta.z;
            delta.tangent[0] = tangentDelta.x;
            delta.tangent[1] = tangentDelta.y;
            delta.tangent[2] = tangentDelta.z;
        }
        out.write(reinterpret_cast<const char*>(deltas.data()),
                  static_cast<std::streamsize>(deltas.size() * sizeof(FzMorphDelta)));
    }
    return out.good();
}

// ── スケルトン書き出し ────────────────────────────────────────────────────
struct NodeEntry {
    std::string name;
    int         parentIndex = -1;
    int         boneIndex   = -1;
    float       bindT[3]{}, bindR[4]{0,0,0,1}, bindS[3]{1,1,1};
    float       localTransform[16]{};
    std::vector<int> children;
};
struct BoneEntry {
    std::string name;
    int         nodeIndex = -1;
    float       offsetMatrix[16]{};
};

std::unordered_map<std::string, uint32_t> CollectGlobalBoneIndices(const aiScene* scene)
{
    std::unordered_map<std::string, uint32_t> boneMap;
    if (!scene) return boneMap;

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi) {
            const std::string name = mesh->mBones[bi]->mName.C_Str();
            if (boneMap.find(name) == boneMap.end())
                boneMap[name] = static_cast<uint32_t>(boneMap.size());
        }
    }
    return boneMap;
}

void CopyMatrix(const aiMatrix4x4& src, float dst[16], float us = 1.0f)
{
    dst[ 0]=src.a1; dst[ 1]=src.a2; dst[ 2]=src.a3; dst[ 3]=src.a4*us;
    dst[ 4]=src.b1; dst[ 5]=src.b2; dst[ 6]=src.b3; dst[ 7]=src.b4*us;
    dst[ 8]=src.c1; dst[ 9]=src.c2; dst[10]=src.c3; dst[11]=src.c4*us;
    dst[12]=src.d1; dst[13]=src.d2; dst[14]=src.d3; dst[15]=src.d4;
}

void TraverseNodes(const aiNode* node, int parentIdx, float us,
                   std::unordered_map<std::string,int>& indexMap,
                   std::vector<NodeEntry>& nodes)
{
    const int myIdx = static_cast<int>(nodes.size());
    nodes.emplace_back();
    NodeEntry& e = nodes.back();
    e.name        = node->mName.C_Str();
    e.parentIndex = parentIdx;

    aiVector3D pos, scale; aiQuaternion rot;
    node->mTransformation.Decompose(scale, rot, pos);
    e.bindT[0]=pos.x*us; e.bindT[1]=pos.y*us; e.bindT[2]=pos.z*us;
    e.bindR[0]=rot.x; e.bindR[1]=rot.y; e.bindR[2]=rot.z; e.bindR[3]=rot.w;
    e.bindS[0]=scale.x; e.bindS[1]=scale.y; e.bindS[2]=scale.z;
    CopyMatrix(node->mTransformation, e.localTransform, us);

    indexMap[e.name] = myIdx;
    if (parentIdx >= 0)
        nodes[static_cast<size_t>(parentIdx)].children.push_back(myIdx);

    for (uint32_t c=0; c<node->mNumChildren; ++c)
        TraverseNodes(node->mChildren[c], myIdx, us, indexMap, nodes);
}

// 頂点/法線/接線へバインド回転を焼き込む (float[3] インプレース)。
void RotateInPlace(float v[3], const aiQuaternion& q)
{
    const aiVector3D r = q.Rotate(aiVector3D(v[0], v[1], v[2]));
    v[0] = r.x; v[1] = r.y; v[2] = r.z;
}

// バインド回転 R の逆行列。offsetMatrix を offset·R⁻¹ へ補正するのに使う。
aiMatrix4x4 InverseBakeMatrix(const float q[4])
{
    const aiQuaternion inv(q[3], -q[0], -q[1], -q[2]); // 単位クォータニオンの共役
    return aiMatrix4x4(inv.GetMatrix());
}

bool WriteSkeleton(std::ofstream& out, const aiScene* scene, float us,
                   const float bakeRotation[4])
{
    using namespace asset;
    const aiMatrix4x4 bakeInv = InverseBakeMatrix(bakeRotation);

    std::unordered_map<std::string,int> nodeIndexMap;
    std::vector<NodeEntry> nodes;
    nodes.reserve(128);
    TraverseNodes(scene->mRootNode, -1, us, nodeIndexMap, nodes);

    // ボーン収集 (全メッシュ分をマージ)
    std::vector<BoneEntry> bones;
    std::unordered_map<std::string, size_t> boneMap;
    for (uint32_t mi=0; mi<scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        for (uint32_t bi=0; bi<mesh->mNumBones; ++bi) {
            const aiBone* bone = mesh->mBones[bi];
            const std::string bname = bone->mName.C_Str();
            if (boneMap.count(bname)) continue;
            boneMap[bname] = bones.size();
            BoneEntry be;
            be.name = bname;
            be.nodeIndex = nodeIndexMap.count(bname) ? nodeIndexMap.at(bname) : -1;
            // offset·R⁻¹: 頂点側に焼いた R を打ち消し、アニメ結果を不変に保つ
            CopyMatrix(bone->mOffsetMatrix * bakeInv, be.offsetMatrix, us);
            bones.push_back(std::move(be));
            if (be.nodeIndex >= 0)
                nodes[static_cast<size_t>(be.nodeIndex)].boneIndex = static_cast<int>(bones.size()-1);
        }
    }

    // ルートノードを探す
    int rootIdx = 0; // デフォルトは 0 番
    for (size_t ni=0; ni<nodes.size(); ++ni)
        if (nodes[ni].parentIndex < 0) { rootIdx = static_cast<int>(ni); break; }

    // ルートの逆変換行列
    aiMatrix4x4 rootInv = scene->mRootNode->mTransformation;
    rootInv.Inverse();
    float rootInvData[16];
    CopyMatrix(rootInv, rootInvData, 1.0f); // 回転のみなのでスケールしない

    FzSkelHeader skelHdr{};
    skelHdr.magic[0]='F'; skelHdr.magic[1]='Z'; skelHdr.magic[2]='S'; skelHdr.magic[3]='K';
    skelHdr.version       = FZSKEL_VERSION;
    skelHdr.rootNodeIndex = rootIdx;
    skelHdr.nodeCount     = static_cast<uint32_t>(nodes.size());
    skelHdr.boneCount     = static_cast<uint32_t>(bones.size());
    std::memcpy(skelHdr.rootInverse, rootInvData, sizeof(rootInvData));
    out.write(reinterpret_cast<const char*>(&skelHdr), sizeof(skelHdr));

    for (const auto& n : nodes) {
        FzSkeletonNodeData nd{};
        const size_t nlen = std::min(n.name.size(), sizeof(nd.name)-1);
        std::memcpy(nd.name, n.name.data(), nlen);
        nd.parentIndex = n.parentIndex;
        nd.boneIndex   = n.boneIndex;
        std::memcpy(nd.bindTranslation,   n.bindT,         sizeof(nd.bindTranslation));
        std::memcpy(nd.bindRotation,      n.bindR,         sizeof(nd.bindRotation));
        std::memcpy(nd.bindScale,         n.bindS,         sizeof(nd.bindScale));
        std::memcpy(nd.localBindTransform,n.localTransform,sizeof(nd.localBindTransform));
        nd.childCount = static_cast<uint32_t>(n.children.size());
        out.write(reinterpret_cast<const char*>(&nd), sizeof(nd));
        out.write(reinterpret_cast<const char*>(n.children.data()),
                  static_cast<std::streamsize>(n.children.size() * sizeof(int32_t)));
    }

    for (const auto& b : bones) {
        FzBoneData bd{};
        const size_t blen = std::min(b.name.size(), sizeof(bd.name)-1);
        std::memcpy(bd.name, b.name.data(), blen);
        bd.nodeIndex = b.nodeIndex;
        std::memcpy(bd.offsetMatrix, b.offsetMatrix, sizeof(bd.offsetMatrix));
        out.write(reinterpret_cast<const char*>(&bd), sizeof(bd));
    }

    return out.good();
}

bool WriteMergedMesh(
    const std::string& outputPath,
    const aiScene* scene,
    float unitScale,
    const std::vector<std::string>& selectedMeshNames,
    const std::unordered_map<uint32_t, aiMatrix4x4>* meshTransforms = nullptr,
    const FbxImportContext* ctx = nullptr)
{
    using namespace asset;

    auto isMeshSelected = [&](const aiMesh* mesh) -> bool {
        if (selectedMeshNames.empty()) return true;
        const std::string name = mesh->mName.C_Str();
        for (const auto& selected : selectedMeshNames) {
            if (selected == name) return true;
        }
        return false;
    };

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(4096);
    indices.reserve(8192);

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!isMeshSelected(mesh) || mesh->mNumVertices == 0) continue;

        const uint32_t baseVertex = static_cast<uint32_t>(vertices.size());
        const auto transformIt = meshTransforms ? meshTransforms->find(mi) : std::unordered_map<uint32_t, aiMatrix4x4>::const_iterator{};
        const bool applyNodeTransform = meshTransforms && transformIt != meshTransforms->end();
        const aiMatrix4x4 transform = applyNodeTransform ? transformIt->second : aiMatrix4x4();
        const bool applyStaticAxisFix = ctx && ctx->applyStaticNodeTransforms;
        const aiQuaternion axisInvQ;
        const float axisInvS = ctx ? (1.0f / ctx->axisFixScale) : 1.0f;
        // スキンメッシュは .fzasset 側と同じバインド回転を焼く。ここを揃えないと
        // 統合 .mesh (CPU 側コピー) だけ Z-up のまま残る。
        const bool bakeThisMesh = ctx && mesh->HasBones();
        const aiQuaternion bakeQ = bakeThisMesh
            ? aiQuaternion(ctx->bindBakeRotation[3], ctx->bindBakeRotation[0],
                           ctx->bindBakeRotation[1], ctx->bindBakeRotation[2])
            : aiQuaternion();
        for (uint32_t vi = 0; vi < mesh->mNumVertices; ++vi) {
            Vertex v = applyStaticAxisFix
                ? ConvertVertexWithStaticAxisFix(mesh, vi, unitScale, transform, axisInvQ, axisInvS)
                : (applyNodeTransform
                    ? ConvertVertexWithNodeTransform(mesh, vi, unitScale, transform)
                    : ConvertVertex(mesh, vi, unitScale));
            if (bakeThisMesh) {
                RotateInPlace(v.position, bakeQ);
                RotateInPlace(v.normal,   bakeQ);
                RotateInPlace(v.tangent,  bakeQ);
            }
            vertices.push_back(v);
        }

        for (uint32_t fi = 0; fi < mesh->mNumFaces; ++fi) {
            const aiFace& face = mesh->mFaces[fi];
            if (face.mNumIndices != 3) continue;
            indices.push_back(baseVertex + face.mIndices[0]);
            indices.push_back(baseVertex + face.mIndices[1]);
            indices.push_back(baseVertex + face.mIndices[2]);
        }
    }

    if (vertices.empty() || indices.empty()) return true;

    const std::string tempOutputPath = outputPath + ".tmp";
    struct TempFileGuard {
        std::string path;
        bool committed = false;
        ~TempFileGuard() {
            if (!committed && !path.empty())
                util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path));
        }
    } tempGuard{ tempOutputPath };

    std::ofstream out(tempOutputPath, std::ios::binary);
    if (!out) return false;

    FzMeshHeader hdr{};
    hdr.magic[0] = 'F';
    hdr.magic[1] = 'Z';
    hdr.magic[2] = 'M';
    hdr.magic[3] = 'H';
    hdr.version = FZMESH_VERSION;
    hdr.flags = 0u;
    hdr.vertexCount = static_cast<uint32_t>(vertices.size());
    hdr.indexCount = static_cast<uint32_t>(indices.size());
    ComputeBoundsV(vertices[0].position, vertices.size(), sizeof(Vertex) / sizeof(float),
                   hdr.boundsCenter, hdr.boundsRadius);

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(vertices.data()),
              static_cast<std::streamsize>(vertices.size() * sizeof(Vertex)));
    out.write(reinterpret_cast<const char*>(indices.data()),
              static_cast<std::streamsize>(indices.size() * sizeof(uint32_t)));
    if (!out.good()) return false;
    out.close();

    util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(outputPath));
    if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(tempOutputPath),
                                  util::FileSystem::PathFromUtf8(outputPath))) {
        util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(tempOutputPath));
        return false;
    }

    tempGuard.committed = true;
    return true;
}

} // namespace

bool ModelSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;

    const aiScene* ms = ctx.meshScene; // 静的: PreTransformVertices 済み
    if (!ms) return false;

    const bool skinned = ctx.hasSkin;
    const auto globalBoneIndices = skinned ? CollectGlobalBoneIndices(ctx.scene) :
                                             std::unordered_map<std::string, uint32_t>{};

    // ── マテリアルスロット名 (材質インデックスの文字列化) ──────────────────
    std::vector<std::string> slotNames(ms->mNumMaterials);
    for (uint32_t matIdx = 0; matIdx < ms->mNumMaterials; ++matIdx) {
        slotNames[matIdx] = "Material_" + std::to_string(matIdx);
        aiString aiName;
        ms->mMaterials[matIdx]->Get(AI_MATKEY_NAME, aiName);
        if (aiName.length > 0) slotNames[matIdx] = aiName.C_Str();
    }

    for (uint32_t mi=0; mi<ms->mNumMeshes; ++mi) {
        const uint32_t matIdx = ms->mMeshes[mi]->mMaterialIndex;
        while (slotNames.size() <= matIdx) slotNames.push_back("Material_" + std::to_string(slotNames.size()));
    }

    // ── 出力パス ────────────────────────────────────────────────────────
    namespace fs = std::filesystem;
    // .fzasset も materials/anims/textures と同じ import 生成物フォルダ内へ置く。
    // WHY: Foo/Foo.fzasset 構造に統一すると、移動・削除・再 import の単位が Foo/ だけで完結する。
    const std::string outputPath = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(ctx.manifestDir) / (ctx.baseName + ".fzasset"));
    const std::string mergedMeshPath = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(ctx.manifestDir) / (ctx.baseName + ".mesh"));
    ctx.outputModelPath = outputPath;
    const std::string tempOutputPath = outputPath + ".tmp";
    struct TempFileGuard {
        std::string path;
        bool committed = false;
        ~TempFileGuard() {
            if (!committed && !path.empty())
                util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path));
        }
    } tempGuard{ tempOutputPath };

    std::ofstream out(tempOutputPath, std::ios::binary);
    if (!out) return false;

    // ── FzModelHeader ────────────────────────────────────────────────────
    FzModelHeader modelHdr{};
    modelHdr.magic[0]='F'; modelHdr.magic[1]='Z'; modelHdr.magic[2]='M'; modelHdr.magic[3]='D';
    modelHdr.version          = FZMODEL_VERSION;
    modelHdr.flags            = skinned ? FZMODEL_FLAG_SKINNED : 0u;
    modelHdr.lodCount         = 1;
    modelHdr.materialSlotCount = static_cast<uint32_t>(slotNames.size());
    out.write(reinterpret_cast<const char*>(&modelHdr), sizeof(modelHdr));

    // ── マテリアルスロット名 ─────────────────────────────────────────────
    for (const auto& name : slotNames) {
        char nameBuf[FZMODEL_SLOT_NAME_LEN]{};
        const size_t len = std::min(name.size(), static_cast<size_t>(FZMODEL_SLOT_NAME_LEN-1));
        std::memcpy(nameBuf, name.data(), len);
        out.write(nameBuf, FZMODEL_SLOT_NAME_LEN);
    }

    // ── LOD0 ─────────────────────────────────────────────────────────────
    // 選択的インポートフィルタ
    auto isMeshSelected = [&](const aiMesh* mesh) -> bool {
        if (ctx.selectedMeshNames.empty()) return true;
        const std::string name = mesh->mName.C_Str();
        for (const auto& n : ctx.selectedMeshNames)
            if (n == name) return true;
        return false;
    };

    std::unordered_map<uint32_t, aiMatrix4x4> staticMeshTransforms;
    if (ctx.applyStaticNodeTransforms && ms && ms->mRootNode)
        CollectMeshNodeTransforms(ms->mRootNode, aiMatrix4x4(), staticMeshTransforms);

    uint32_t submeshCount = 0;
    for (uint32_t mi=0; mi<ms->mNumMeshes; ++mi)
        if (isMeshSelected(ms->mMeshes[mi]) && ms->mMeshes[mi]->mNumVertices > 0) ++submeshCount;

    FzLodHeader lodHdr{ 0.0f, submeshCount };
    out.write(reinterpret_cast<const char*>(&lodHdr), sizeof(lodHdr));

    // モデル全体のバウンズは各サブメッシュの境界球を包含して求め、
    // 全サブメッシュを書き終えてからヘッダーへ書き戻す (ヘッダーはファイル先頭で
    // 既に出力済みのため、この時点では値が確定していない)。
    // WHY: 未設定だと center=(0,0,0) / radius=0 のままになり、視錐台カリングが
    //      原点の点として判定してモデルが消える。
    bool  boundsValid = false;
    float boundsMin[3]{}, boundsMax[3]{};
    auto accumulateBounds = [&](const float center[3], float radius) {
        if (radius < 0.0f) return;
        for (int a = 0; a < 3; ++a) {
            const float lo = center[a] - radius;
            const float hi = center[a] + radius;
            if (!boundsValid) { boundsMin[a] = lo; boundsMax[a] = hi; }
            else {
                boundsMin[a] = std::min(boundsMin[a], lo);
                boundsMax[a] = std::max(boundsMax[a], hi);
            }
        }
        boundsValid = true;
    };

    for (uint32_t mi=0; mi<ms->mNumMeshes; ++mi) {
        const aiMesh* mesh = ms->mMeshes[mi];
        if (!isMeshSelected(mesh)) continue;
        if (mesh->mNumVertices == 0) continue;

        // インデックス
        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
        for (uint32_t fi=0; fi<mesh->mNumFaces; ++fi) {
            const aiFace& f = mesh->mFaces[fi];
            if (f.mNumIndices != 3) continue;
            indices.push_back(f.mIndices[0]);
            indices.push_back(f.mIndices[1]);
            indices.push_back(f.mIndices[2]);
        }

        FzSubmeshHeader smHdr{};
        smHdr.materialSlotIndex = mesh->mMaterialIndex < static_cast<uint32_t>(slotNames.size())
                                  ? mesh->mMaterialIndex : 0u;
        smHdr.vertexCount  = mesh->mNumVertices;
        smHdr.indexCount   = static_cast<uint32_t>(indices.size());
        FzSubmeshExtensionV3 smExt{};
        smExt.morphTargetCount = mesh->mNumAnimMeshes;
        const std::string meshName = mesh->mName.length > 0
            ? mesh->mName.C_Str() : ("Mesh_" + std::to_string(mi));
        const size_t meshNameLength = std::min(meshName.size(), sizeof(smExt.name) - 1);
        std::memcpy(smExt.name, meshName.data(), meshNameLength);

        if (!skinned || !mesh->HasBones()) {
            smHdr.vertexFormat = 0;
            std::vector<Vertex> verts(mesh->mNumVertices);
            const auto transformIt = staticMeshTransforms.find(mi);
            const bool applyNodeTransform = transformIt != staticMeshTransforms.end();
            const aiMatrix4x4 transform = applyNodeTransform ? transformIt->second : aiMatrix4x4();
            const aiQuaternion axisInvQ;
            const float axisInvS = 1.0f / ctx.axisFixScale;
            for (uint32_t i=0; i<mesh->mNumVertices; ++i) {
                verts[i] = ctx.applyStaticNodeTransforms
                    ? ConvertVertexWithStaticAxisFix(mesh, i, ctx.unitScale, transform, axisInvQ, axisInvS)
                    : (applyNodeTransform
                        ? ConvertVertexWithNodeTransform(mesh, i, ctx.unitScale, transform)
                        : ConvertVertex(mesh, i, ctx.unitScale));
            }
            ComputeBoundsV(verts[0].position, verts.size(), sizeof(Vertex)/sizeof(float),
                           smHdr.boundsCenter, smHdr.boundsRadius);
            accumulateBounds(smHdr.boundsCenter, smHdr.boundsRadius);
            out.write(reinterpret_cast<const char*>(&smHdr), sizeof(smHdr));
            out.write(reinterpret_cast<const char*>(&smExt), sizeof(smExt));
            out.write(reinterpret_cast<const char*>(verts.data()),
                      static_cast<std::streamsize>(verts.size() * sizeof(Vertex)));
        } else {
            smHdr.vertexFormat = 1;
            std::vector<Influence> infl(mesh->mNumVertices);
            for (uint32_t bi=0; bi<mesh->mNumBones; ++bi) {
                const aiBone* bone = mesh->mBones[bi];
                const auto boneIt = globalBoneIndices.find(bone->mName.C_Str());
                if (boneIt == globalBoneIndices.end()) continue;
                for (uint32_t wi=0; wi<bone->mNumWeights; ++wi)
                    infl[bone->mWeights[wi].mVertexId].Add(boneIt->second, bone->mWeights[wi].mWeight);
            }
            for (auto& inf : infl) inf.Normalize();

            // バインド回転を頂点へ焼き込む (offsetMatrix 側で R⁻¹ を打ち消し済み)。
            // これで「ボーン行列 = identity」がそのままバインドポーズになり、
            // サムネイル / AnimatorComponent 無しの描画でも正しい向きになる。
            const aiQuaternion bakeQ(ctx.bindBakeRotation[3], ctx.bindBakeRotation[0],
                                     ctx.bindBakeRotation[1], ctx.bindBakeRotation[2]);
            std::vector<SkinnedVertex> verts(mesh->mNumVertices);
            for (uint32_t i=0; i<mesh->mNumVertices; ++i) {
                Vertex base = ConvertVertex(mesh, i, ctx.unitScale);
                RotateInPlace(base.position, bakeQ);
                RotateInPlace(base.normal,   bakeQ);
                RotateInPlace(base.tangent,  bakeQ);
                std::memcpy(verts[i].position,    base.position, sizeof(base.position));
                std::memcpy(verts[i].normal,      base.normal,   sizeof(base.normal));
                std::memcpy(verts[i].tangent,     base.tangent,  sizeof(base.tangent));
                std::memcpy(verts[i].uv,          base.uv,       sizeof(base.uv));
                std::memcpy(verts[i].boneIndices, infl[i].idx.data(), 4*sizeof(uint32_t));
                std::memcpy(verts[i].boneWeights, infl[i].wgt.data(), 4*sizeof(float));
            }
            ComputeBoundsV(verts[0].position, verts.size(), sizeof(SkinnedVertex)/sizeof(float),
                           smHdr.boundsCenter, smHdr.boundsRadius);
            accumulateBounds(smHdr.boundsCenter, smHdr.boundsRadius);
            out.write(reinterpret_cast<const char*>(&smHdr), sizeof(smHdr));
            out.write(reinterpret_cast<const char*>(&smExt), sizeof(smExt));
            out.write(reinterpret_cast<const char*>(verts.data()),
                      static_cast<std::streamsize>(verts.size() * sizeof(SkinnedVertex)));
        }
        out.write(reinterpret_cast<const char*>(indices.data()),
                  static_cast<std::streamsize>(indices.size() * sizeof(uint32_t)));
        const auto transformIt = staticMeshTransforms.find(mi);
        const bool applyNodeTransform = transformIt != staticMeshTransforms.end();
        const aiMatrix4x4 transform = applyNodeTransform ? transformIt->second : aiMatrix4x4();
        const aiQuaternion axisInvQ;
        // 静的メッシュは transform 側で既に回っているのでモーフの追加回転は不要。
        // スキンメッシュのみ、頂点へ焼いた R を同じくデルタへ適用する。
        const bool meshIsSkinned = skinned && mesh->HasBones();
        const aiQuaternion morphBakeQ = meshIsSkinned
            ? aiQuaternion(ctx.bindBakeRotation[3], ctx.bindBakeRotation[0],
                           ctx.bindBakeRotation[1], ctx.bindBakeRotation[2])
            : aiQuaternion();
        if (!WriteMorphTargets(out, *mesh, ctx.unitScale, applyNodeTransform, transform,
                               ctx.applyStaticNodeTransforms, axisInvQ,
                               1.0f / ctx.axisFixScale, morphBakeQ)) return false;
    }

    // ── スケルトン ────────────────────────────────────────────────────────
    if (skinned && ctx.scene) {
        if (!WriteSkeleton(out, ctx.scene, ctx.unitScale, ctx.bindBakeRotation)) return false;
    }

    // ── モデル全体バウンズをヘッダーへ書き戻す ────────────────────────────
    // スキンドの場合はバインド姿勢の頂点から求めた保守的な球。アニメで
    // これを超える動きをするクリップは別途スケール係数で膨らませる想定。
    if (boundsValid) {
        float radiusSq = 0.0f;
        for (int a = 0; a < 3; ++a) {
            modelHdr.boundsCenter[a] = (boundsMin[a] + boundsMax[a]) * 0.5f;
            const float half = (boundsMax[a] - boundsMin[a]) * 0.5f;
            radiusSq += half * half;
        }
        modelHdr.boundsRadius = std::sqrt(radiusSq);

        const std::streampos endPos = out.tellp();
        out.seekp(0, std::ios::beg);
        out.write(reinterpret_cast<const char*>(&modelHdr), sizeof(modelHdr));
        out.seekp(endPos, std::ios::beg);
    }

    if (!out.good()) return false;
    out.close();

    util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(outputPath));
    if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(tempOutputPath),
                                  util::FileSystem::PathFromUtf8(outputPath))) {
        util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(tempOutputPath));
        return false;
    }
    tempGuard.committed = true;

    return WriteMergedMesh(
        mergedMeshPath,
        ms,
        ctx.unitScale,
        ctx.selectedMeshNames,
        ctx.applyStaticNodeTransforms ? &staticMeshTransforms : nullptr,
        &ctx);
}

} // namespace fbzz::editor
