/// @file    SkelSubExporter.cpp
/// @brief   FBX シーンノード階層を [baseName].skel バイナリに書き出す。
/// @author  Hasegawa Jin
/// @date    2026-06-19
///
/// スキンあり FBX: ノード階層 + メッシュボーンのオフセット行列を含む。
/// スキンなし FBX: ノード階層のみ (boneCount=0) を出力する。
/// → Mixamo アニメーション専用 FBX はメッシュボーンを持たないため、
/// アニメーショントラック名と骨ノード名を対応付けるだけの骨階層を提供する。
#include <Editor/Import/SkelSubExporter.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/scene.h>
#include "AssimpNodeExport.hpp"
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

namespace {
using fbxexport::CopyMatrix;
using fbxexport::TraverseNodes;

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





} /// @note namespace

bool SkelSubExporter::Export(FbxImportContext& ctx)
{
    using namespace asset;
    const aiScene* scene = ctx.scene;
    /// @note シーンなしは正常スキップ
    if (!scene || !scene->mRootNode) return true;

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

    /// @note ModelSubExporter がスキンメッシュ頂点へ焼き込んだバインド回転 R を
    ///       offsetMatrix 側で打ち消す (offset·R⁻¹)。両者は必ず同じ R を使うこと。
    const aiQuaternion bakeInvQ(ctx.bindBakeRotation[3], -ctx.bindBakeRotation[0],
                                -ctx.bindBakeRotation[1], -ctx.bindBakeRotation[2]);
    const aiMatrix4x4 bakeInv(bakeInvQ.GetMatrix());

    /// @note ボーン収集 (全メッシュのボーン情報をマージ)
    ///       スキンなし FBX ではボーンがないため空になる。その場合でもノード階層は書き出す。
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
            CopyMatrix(bone->mOffsetMatrix * bakeInv, be.offsetMatrix, ctx.unitScale);
            /// @note 対応ノードに boneIndex を書き込む
            if (be.nodeIndex >= 0)
                nodes[static_cast<size_t>(be.nodeIndex)].boneIndex =
                    static_cast<int>(bones.size());
            bones.push_back(std::move(be));
        }
    }

    /// @note ルートノードインデックスと逆変換行列
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

} /// @note namespace fbzz::editor
