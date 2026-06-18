// FBZZ Engine
// FzModelFormat.hpp | fbzz::asset
// .model バイナリのオンディスクレイアウト定義
// メッシュ複数 + LOD + スケルトンを 1 ファイルに統合する。
// 旧 FzAssetFormat.hpp の FZMH/FZSK 部分を置き換える。
//
// レイアウト:
//   FzModelHeader
//   materialSlotNames: materialSlotCount × 64 バイト (null 終端)
//   LOD[0..lodCount-1]:
//     FzLodHeader
//     FzSubmeshHeader[0..submeshCount-1]
//       頂点データ (Vertex or SkinnedVertex)
//       インデックスデータ (uint32 × indexCount)
//   スケルトン (SKINNED フラグ時):
//     FzSkelHeader
//     FzSkeletonNodeData[] (各ノードの childCount × int32 が直後に続く)
//     FzBoneData[]
#pragma once
#include <cstdint>

namespace fbzz::asset {

constexpr uint32_t FZMODEL_VERSION      = 1;
constexpr uint32_t FZMODEL_FLAG_SKINNED = 1u << 0;
constexpr uint32_t FZMODEL_FLAG_LOD     = 1u << 1;
constexpr uint32_t FZMODEL_SLOT_NAME_LEN = 64;

struct FzModelHeader {
    char     magic[4];           // "FZMD"
    uint32_t version;
    uint32_t flags;              // FZMODEL_FLAG_*
    uint32_t lodCount;
    uint32_t materialSlotCount;
    float    boundsCenter[3];
    float    boundsRadius;
    uint32_t _pad[3];
};
static_assert(sizeof(FzModelHeader) == 48, "FzModelHeader size mismatch");

struct FzLodHeader {
    float    screenSizeThreshold; // このLODを使う画面占有率の下限 (0.0 = 常時使用)
    uint32_t submeshCount;
};
static_assert(sizeof(FzLodHeader) == 8, "FzLodHeader size mismatch");

struct FzSubmeshHeader {
    uint32_t materialSlotIndex;
    uint32_t vertexFormat;       // 0 = Vertex (static), 1 = SkinnedVertex
    uint32_t vertexCount;
    uint32_t indexCount;
    float    boundsCenter[3];
    float    boundsRadius;
};
static_assert(sizeof(FzSubmeshHeader) == 32, "FzSubmeshHeader size mismatch");

// スケルトンヘッダー (旧 FzSkelHeader と同じレイアウト)
constexpr uint32_t FZSKEL_VERSION = 1;

struct FzSkelHeader {
    char     magic[4];           // "FZSK"
    uint32_t version;
    int32_t  rootNodeIndex;
    uint32_t nodeCount;
    uint32_t boneCount;
    float    rootInverse[16];    // row-major Matrix4
};
static_assert(sizeof(FzSkelHeader) == 84, "FzSkelHeader size mismatch");

struct FzSkeletonNodeData {
    char     name[64];
    int32_t  parentIndex;
    int32_t  boneIndex;
    float    bindTranslation[3];
    float    bindRotation[4];    // x,y,z,w
    float    bindScale[3];
    float    localBindTransform[16];
    uint32_t childCount;
    // 直後: childCount × int32_t
};
static_assert(sizeof(FzSkeletonNodeData) == 180, "FzSkeletonNodeData size mismatch");

struct FzBoneData {
    char     name[64];
    int32_t  nodeIndex;
    float    offsetMatrix[16];
    float    _pad;
};
static_assert(sizeof(FzBoneData) == 136, "FzBoneData size mismatch");

} // namespace fbzz::asset
