// FBZZ Engine
// FzModelSubExporter.cpp | fbzz::editor
// FBX → .model (FZMD) バイナリ + スケルトン埋め込み
// メッシュ・スケルトンを 1 つの .model ファイルに統合する。
// 旧パイプライン: FzMeshExporter (.mesh) + FzSkeletonExporter (.skel) → .asset マニフェスト
// 新パイプライン: このクラスが .model 1 ファイルに完結させる。
#include <Editor/Import/FzModelSubExporter.hpp>
#include <Engine/Asset/FzModelFormat.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/mesh.h>
#include <assimp/scene.h>
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

bool WriteSkeleton(std::ofstream& out, const aiScene* scene, float us)
{
    using namespace asset;

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
            CopyMatrix(bone->mOffsetMatrix, be.offsetMatrix, us);
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

} // namespace

bool FzModelSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;

    const aiScene* ms = ctx.meshScene; // 静的: PreTransformVertices 済み
    if (!ms) return false;

    const bool skinned = ctx.hasSkin;

    // ── マテリアルスロット名 (材質インデックスの文字列化) ──────────────────
    std::vector<std::string> slotNames;
    for (uint32_t mi=0; mi<ms->mNumMeshes; ++mi) {
        const uint32_t matIdx = ms->mMeshes[mi]->mMaterialIndex;
        while (slotNames.size() <= matIdx) slotNames.push_back("slot_" + std::to_string(slotNames.size()));
        // マテリアル名で上書き
        if (matIdx < ms->mNumMaterials) {
            aiString aiName;
            ms->mMaterials[matIdx]->Get(AI_MATKEY_NAME, aiName);
            if (aiName.length > 0) slotNames[matIdx] = aiName.C_Str();
        }
    }

    // ── 出力パス ────────────────────────────────────────────────────────
    namespace fs = std::filesystem;
    const std::string outputPath = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(ctx.manifestDir) / (ctx.baseName + ".model"));
    ctx.outputModelPath = outputPath;

    std::ofstream out(outputPath, std::ios::binary);
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

    uint32_t submeshCount = 0;
    for (uint32_t mi=0; mi<ms->mNumMeshes; ++mi)
        if (isMeshSelected(ms->mMeshes[mi])) ++submeshCount;

    FzLodHeader lodHdr{ 0.0f, submeshCount };
    out.write(reinterpret_cast<const char*>(&lodHdr), sizeof(lodHdr));

    for (uint32_t mi=0; mi<ms->mNumMeshes; ++mi) {
        const aiMesh* mesh = ms->mMeshes[mi];
        if (!isMeshSelected(mesh)) continue;

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

        if (!skinned || !mesh->HasBones()) {
            smHdr.vertexFormat = 0;
            std::vector<Vertex> verts(mesh->mNumVertices);
            for (uint32_t i=0; i<mesh->mNumVertices; ++i)
                verts[i] = ConvertVertex(mesh, i, ctx.unitScale);
            ComputeBoundsV(verts[0].position, verts.size(), sizeof(Vertex)/sizeof(float),
                           smHdr.boundsCenter, smHdr.boundsRadius);
            out.write(reinterpret_cast<const char*>(&smHdr), sizeof(smHdr));
            out.write(reinterpret_cast<const char*>(verts.data()),
                      static_cast<std::streamsize>(verts.size() * sizeof(Vertex)));
        } else {
            smHdr.vertexFormat = 1;
            std::vector<Influence> infl(mesh->mNumVertices);
            for (uint32_t bi=0; bi<mesh->mNumBones; ++bi) {
                const aiBone* bone = mesh->mBones[bi];
                for (uint32_t wi=0; wi<bone->mNumWeights; ++wi)
                    infl[bone->mWeights[wi].mVertexId].Add(bi, bone->mWeights[wi].mWeight);
            }
            for (auto& inf : infl) inf.Normalize();

            std::vector<SkinnedVertex> verts(mesh->mNumVertices);
            for (uint32_t i=0; i<mesh->mNumVertices; ++i) {
                const Vertex base = ConvertVertex(mesh, i, ctx.unitScale);
                std::memcpy(verts[i].position,    base.position, sizeof(base.position));
                std::memcpy(verts[i].normal,      base.normal,   sizeof(base.normal));
                std::memcpy(verts[i].tangent,     base.tangent,  sizeof(base.tangent));
                std::memcpy(verts[i].uv,          base.uv,       sizeof(base.uv));
                std::memcpy(verts[i].boneIndices, infl[i].idx.data(), 4*sizeof(uint32_t));
                std::memcpy(verts[i].boneWeights, infl[i].wgt.data(), 4*sizeof(float));
            }
            ComputeBoundsV(verts[0].position, verts.size(), sizeof(SkinnedVertex)/sizeof(float),
                           smHdr.boundsCenter, smHdr.boundsRadius);
            out.write(reinterpret_cast<const char*>(&smHdr), sizeof(smHdr));
            out.write(reinterpret_cast<const char*>(verts.data()),
                      static_cast<std::streamsize>(verts.size() * sizeof(SkinnedVertex)));
        }
        out.write(reinterpret_cast<const char*>(indices.data()),
                  static_cast<std::streamsize>(indices.size() * sizeof(uint32_t)));
    }

    // ── スケルトン ────────────────────────────────────────────────────────
    if (skinned && ctx.scene) {
        if (!WriteSkeleton(out, ctx.scene, ctx.unitScale)) return false;
    }

    if (!out.good()) return false;
    return true;
}

} // namespace fbzz::editor
