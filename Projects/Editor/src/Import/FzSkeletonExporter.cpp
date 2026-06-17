// FBZZ Engine
// FzSkeletonExporter.cpp | fbzz::editor
// aiScene のスケルトン情報 → .skel バイナリ書き出し
#include <Editor/Import/FzSkeletonExporter.hpp>
#include <Engine/Asset/FzAssetFormat.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <assimp/scene.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

namespace {

struct NodeEntry {
    std::string  name;
    int          parentIndex = -1;
    int          boneIndex   = -1;
    float        bindTranslation[3]{};
    float        bindRotation[4]{ 0,0,0,1 };
    float        bindScale[3]{ 1,1,1 };
    float        localBindTransform[16]{};
    std::vector<int> children;
};

struct BoneEntry {
    std::string name;
    int         nodeIndex = -1;
    float       offsetMatrix[16]{};
};

// aiMatrix4x4 → float[16] row-major
void CopyMatrix(const aiMatrix4x4& src, float dst[16], float unitScale = 1.0f)
{
    dst[ 0] = src.a1; dst[ 1] = src.a2; dst[ 2] = src.a3; dst[ 3] = src.a4 * unitScale;
    dst[ 4] = src.b1; dst[ 5] = src.b2; dst[ 6] = src.b3; dst[ 7] = src.b4 * unitScale;
    dst[ 8] = src.c1; dst[ 9] = src.c2; dst[10] = src.c3; dst[11] = src.c4 * unitScale;
    dst[12] = src.d1; dst[13] = src.d2; dst[14] = src.d3; dst[15] = src.d4;
}

// aiNode 木を DFS で走査してノード一覧を構築する
void TraverseNodes(const aiNode* node,
                   int parentIndex,
                   float unitScale,
                   std::unordered_map<std::string, int>& nodeIndexMap,
                   std::vector<NodeEntry>& nodes)
{
    const int myIndex = static_cast<int>(nodes.size());
    nodes.emplace_back();
    NodeEntry& entry = nodes.back();

    entry.name        = node->mName.C_Str();
    entry.parentIndex = parentIndex;

    // デコンポーズでバインドポーズ TRS を取得
    aiVector3D   pos, scale;
    aiQuaternion rot;
    node->mTransformation.Decompose(scale, rot, pos);
    entry.bindTranslation[0] = pos.x   * unitScale;
    entry.bindTranslation[1] = pos.y   * unitScale;
    entry.bindTranslation[2] = pos.z   * unitScale;
    entry.bindRotation[0]    = rot.x;
    entry.bindRotation[1]    = rot.y;
    entry.bindRotation[2]    = rot.z;
    entry.bindRotation[3]    = rot.w;
    entry.bindScale[0]       = scale.x;
    entry.bindScale[1]       = scale.y;
    entry.bindScale[2]       = scale.z;
    CopyMatrix(node->mTransformation, entry.localBindTransform, unitScale);

    nodeIndexMap[entry.name] = myIndex;

    if (parentIndex >= 0)
        nodes[static_cast<size_t>(parentIndex)].children.push_back(myIndex);

    for (uint32_t ci = 0; ci < node->mNumChildren; ++ci)
        TraverseNodes(node->mChildren[ci], myIndex, unitScale, nodeIndexMap, nodes);
}

// identity 行列を float[16] に設定する
void SetIdentity(float m[16])
{
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

} // namespace

bool FzSkeletonExporter::Export(const aiScene* scene,
                                 float unitScale,
                                 const std::string& outputPath)
{
    using namespace asset;

    std::vector<NodeEntry> nodes;
    std::unordered_map<std::string, int> nodeIndexMap;

    // ── ノード木の走査 ───────────────────────────────────────────────────
    if (scene->mRootNode)
        TraverseNodes(scene->mRootNode, -1, unitScale, nodeIndexMap, nodes);

    // ── ボーン一覧の構築 ─────────────────────────────────────────────────
    std::vector<BoneEntry> bones;
    std::unordered_map<std::string, int> boneIndexMap;

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi) {
            const aiBone* ab = mesh->mBones[bi];
            const std::string boneName = ab->mName.C_Str();
            if (boneIndexMap.count(boneName)) continue;

            BoneEntry be;
            be.name = boneName;

            auto it = nodeIndexMap.find(boneName);
            be.nodeIndex = (it != nodeIndexMap.end()) ? it->second : -1;
            CopyMatrix(ab->mOffsetMatrix, be.offsetMatrix, unitScale);

            boneIndexMap[boneName] = static_cast<int>(bones.size());

            // nodeEntry に boneIndex を書き込む
            if (be.nodeIndex >= 0)
                nodes[static_cast<size_t>(be.nodeIndex)].boneIndex =
                    static_cast<int>(bones.size());

            bones.push_back(std::move(be));
        }
    }

    // ── ルートノードインデックスを特定 ───────────────────────────────────
    const int rootNodeIndex = nodes.empty() ? -1 : 0;

    // ── rootInverseTransform ─────────────────────────────────────────────
    // WHY: FBX のグローバル変換を打ち消すために必要。
    //      ルートノードのグローバル変換の逆行列を設定する。
    float rootInverse[16];
    if (scene->mRootNode) {
        aiMatrix4x4 inv = scene->mRootNode->mTransformation;
        inv.Inverse();
        CopyMatrix(inv, rootInverse, unitScale);
    } else {
        SetIdentity(rootInverse);
    }

    // ── バイナリ書き出し ─────────────────────────────────────────────────
    auto out = util::FileSystem::OpenBinaryWriter(util::FileSystem::PathFromUtf8(outputPath));
    if (!out) {
        FBZZ_LOG_ERROR("FzSkeletonExporter: cannot open [%s]", outputPath.c_str());
        return false;
    }

    FzSkelHeader hdr{};
    hdr.magic[0] = 'F'; hdr.magic[1] = 'Z'; hdr.magic[2] = 'S'; hdr.magic[3] = 'K';
    hdr.version       = FZSKEL_VERSION;
    hdr.rootNodeIndex = rootNodeIndex;
    hdr.nodeCount     = static_cast<uint32_t>(nodes.size());
    hdr.boneCount     = static_cast<uint32_t>(bones.size());
    std::memcpy(hdr.rootInverse, rootInverse, sizeof(rootInverse));
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    // ノードデータ (可変長: 末尾に children int32_t 配列)
    for (const auto& n : nodes) {
        FzSkeletonNodeData nd{};
        std::strncpy(nd.name, n.name.c_str(), sizeof(nd.name) - 1);
        nd.parentIndex = n.parentIndex;
        nd.boneIndex   = n.boneIndex;
        std::memcpy(nd.bindTranslation,     n.bindTranslation,     sizeof(nd.bindTranslation));
        std::memcpy(nd.bindRotation,        n.bindRotation,        sizeof(nd.bindRotation));
        std::memcpy(nd.bindScale,           n.bindScale,           sizeof(nd.bindScale));
        std::memcpy(nd.localBindTransform,  n.localBindTransform,  sizeof(nd.localBindTransform));
        nd.childCount = static_cast<uint32_t>(n.children.size());
        out.write(reinterpret_cast<const char*>(&nd), sizeof(nd));
        out.write(reinterpret_cast<const char*>(n.children.data()),
                  static_cast<std::streamsize>(n.children.size() * sizeof(int32_t)));
    }

    // ボーンデータ
    for (const auto& b : bones) {
        FzBoneData bd{};
        std::strncpy(bd.name, b.name.c_str(), sizeof(bd.name) - 1);
        bd.nodeIndex = b.nodeIndex;
        std::memcpy(bd.offsetMatrix, b.offsetMatrix, sizeof(bd.offsetMatrix));
        out.write(reinterpret_cast<const char*>(&bd), sizeof(bd));
    }

    return out.good();
}

} // namespace fbzz::editor
