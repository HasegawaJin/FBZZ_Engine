// FBZZ Engine
// FzModelFormat.hpp | fbzz::asset
// .fzasset バイナリのオンディスクレイアウト定義
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
#include <Engine/Asset/FzAssetFormat.hpp>
#include <cstdint>

namespace fbzz::asset {

constexpr uint32_t FZMODEL_VERSION      = 3;
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

// v3 では、インポート元の aiMesh::mName をサブメッシュごとに保持する。
// WHY: Scene 配置時に Mesh_0 のようなエンジン都合の名前へ置き換えず、
//      FBX のメッシュ名をそのまま GameObject 名へ反映するため。
struct FzSubmeshExtensionV3 {
    uint32_t morphTargetCount;
    char     name[128];
};

struct FzMorphTargetHeader {
    char name[128];
    uint32_t vertexCount;
};

struct FzMorphDelta {
    float position[3];
    float normal[3];
    float tangent[3];
};
static_assert(sizeof(FzMorphDelta) == 36, "FzMorphDelta size mismatch");

// FzSkelHeader / FzSkeletonNodeData / FzBoneData / FZSKEL_VERSION は
// FzAssetFormat.hpp で定義済み (上記 include 経由で参照可)

} // namespace fbzz::asset
