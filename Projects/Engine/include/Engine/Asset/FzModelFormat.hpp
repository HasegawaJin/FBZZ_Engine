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
//   ノード階層 (v4 以降・NODES フラグ時):
//     FzModelNodeChunkHeader
//     FzModelNodeData[] (各ノードの meshCount × uint32 と childCount × int32 が直後に続く)
#pragma once
#include <Engine/Asset/FzAssetFormat.hpp>
#include <cstdint>

namespace fbzz::asset {

// v4: 末尾に DCC のノード階層チャンク (FZND) を追加。
// WHY 末尾へ足すか: 既存の全オフセットに触れずに拡張できる。読み手は LOD と
//     スケルトンを読み終えた後で、フラグを見てから続きを読むだけでよい。
constexpr uint32_t FZMODEL_VERSION      = 4;
constexpr uint32_t FZMODEL_FLAG_SKINNED = 1u << 0;
constexpr uint32_t FZMODEL_FLAG_LOD     = 1u << 1;
// ノード階層チャンクが存在する。メッシュを持たないモデルでは立たない。
constexpr uint32_t FZMODEL_FLAG_NODES   = 1u << 2;
// ノードの変換が既に頂点へ焼き込まれている。
//
// WHY このフラグが要るか (重要):
//   現状の書き出しは 2 通りとも頂点をモデル空間へ落としている。
//     静的: aiProcess_PreTransformVertices / applyStaticNodeTransforms で頂点へベイク
//     スキンド: 描画はボーンパレット経由で、メッシュノードの変換は使われない
//   それでもノードの TRS は情報として保存する価値がある (将来ベイクを止めたときに必要)。
//   保存された TRS を無条件に GameObject の Transform へ入れると二重変換になるため、
//   「入れてよいか」を配置側が判断できるようにフラグで明示する。これが無いと
//   モデルが原点へ潰れる / 二重に回るという、原因の見えない壊れ方をする。
constexpr uint32_t FZMODEL_FLAG_NODE_TRANSFORMS_BAKED = 1u << 3;
constexpr uint32_t FZMODEL_SLOT_NAME_LEN = 64;
constexpr uint32_t FZMODEL_NODE_NAME_LEN = 64;

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

// ── ノード階層チャンク (v4) ────────────────────────────────────────────
// DCC のノード 1 個 = 配置時の 1 GameObject。ノード内のマテリアル分割は
// meshIndices の複数要素 (= Renderer の submesh 列) として表現する。
struct FzModelNodeChunkHeader {
    char     magic[4];        // "FZND"
    uint32_t nodeCount;
    int32_t  rootNodeIndex;
    uint32_t _pad;
};
static_assert(sizeof(FzModelNodeChunkHeader) == 16, "FzModelNodeChunkHeader size mismatch");

// 1 ノードのシリアライズ形式。
// 直後に meshCount × uint32_t (lods[0].submeshes の添字)、
// 続いて childCount × int32_t が並ぶ。
struct FzModelNodeData {
    char     name[FZMODEL_NODE_NAME_LEN];
    int32_t  parentIndex;
    float    localTranslation[3];
    float    localRotation[4];    // x,y,z,w
    float    localScale[3];
    uint32_t meshCount;
    uint32_t childCount;
};
static_assert(sizeof(FzModelNodeData) == 116, "FzModelNodeData size mismatch");

// FzSkelHeader / FzSkeletonNodeData / FzBoneData / FZSKEL_VERSION は
// FzAssetFormat.hpp で定義済み (上記 include 経由で参照可)

} // namespace fbzz::asset
