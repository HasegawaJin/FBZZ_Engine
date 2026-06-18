// FBZZ Engine
// SkelSubExporter.cpp | fbzz::editor
// FBX シーンノード階層を [baseName].skel バイナリに書き出す。
// スキンあり FBX: ノード階層 + メッシュボーンのオフセット行列を含む。
// スキンなし FBX: ノード階層のみ (boneCount=0) を出力する。
//   → Mixamo アニメーション専用 FBX はメッシュボーンを持たないため、
//     アニメーショントラック名と骨ノード名を対応付けるだけの骨階層を提供する。
#include <Editor/Import/SkelSubExporter.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/scene.h>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

namespace {

struct NodeEntry {
    std::string name;
    int         parentIndex = -1;
    int         boneIndex   = -1;
    float       bindT[3]{};
    float       bindR[4]{};
    float       bindS[3]{ 1,1,1 };
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

    for (uint32_t c = 0; c < node->mNumChildren; ++c)
        TraverseNodes(node->mChildren[c], myIdx, us, indexMap, nodes);
}

} // namespace

bool SkelSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;
    const aiScene* scene = ctx.scene;
    if (!scene || !scene->mRootNode) return true; // シーンなしは正常スキップ

    namespace fs = std::filesystem;
    const std::string outputPath = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(ctx.manifestDir) / (ctx.baseName + ".skel"));
    const std::string tempPath = outputPath + ".tmp";

    struct TempGuard {
        std::string path; bool committed = false;
        ~TempGuard() {
            if (!committed && !path.empty())
                util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(path));
        }
    } guard{ tempPath };

    std::unordered_map<std::string,int> nodeIndexMap;
    std::vector<NodeEntry> nodes;
    nodes.reserve(128);
    TraverseNodes(scene->mRootNode, -1, ctx.unitScale, nodeIndexMap, nodes);

    // ボーン収集 (全メッシュのボーン情報をマージ)
    // スキンなし FBX ではボーンがないため空になる。その場合でもノード階層は書き出す。
    std::vector<BoneEntry> bones;
    std::unordered_map<std::string, size_t> boneMap;
    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi) {
            const aiBone* bone = mesh->mBones[bi];
            const std::string bname = bone->mName.C_Str();
            if (boneMap.count(bname)) continue;
            boneMap[bname] = bones.size();
            BoneEntry be;
            be.name = bname;
            be.nodeIndex = nodeIndexMap.count(bname) ? nodeIndexMap.at(bname) : -1;
            CopyMatrix(bone->mOffsetMatrix, be.offsetMatrix, ctx.unitScale);
            // 対応ノードに boneIndex を書き込む
            if (be.nodeIndex >= 0)
                nodes[static_cast<size_t>(be.nodeIndex)].boneIndex =
                    static_cast<int>(bones.size());
            bones.push_back(std::move(be));
        }
    }

    // ルートノードインデックスと逆変換行列
    int rootIdx = 0;
    for (size_t ni = 0; ni < nodes.size(); ++ni)
        if (nodes[ni].parentIndex < 0) { rootIdx = static_cast<int>(ni); break; }

    aiMatrix4x4 rootInv = scene->mRootNode->mTransformation;
    rootInv.Inverse();
    float rootInvData[16];
    CopyMatrix(rootInv, rootInvData, 1.0f);

    std::ofstream out(tempPath, std::ios::binary);
    if (!out) return false;

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
        std::memcpy(nd.bindTranslation,    n.bindT,         sizeof(nd.bindTranslation));
        std::memcpy(nd.bindRotation,       n.bindR,         sizeof(nd.bindRotation));
        std::memcpy(nd.bindScale,          n.bindS,         sizeof(nd.bindScale));
        std::memcpy(nd.localBindTransform, n.localTransform,sizeof(nd.localBindTransform));
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

    if (!out.good()) return false;
    out.close();

    util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(outputPath));
    if (!util::FileSystem::Rename(util::FileSystem::PathFromUtf8(tempPath),
                                   util::FileSystem::PathFromUtf8(outputPath))) {
        util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(tempPath));
        return false;
    }
    guard.committed = true;
    return true;
}

} // namespace fbzz::editor
