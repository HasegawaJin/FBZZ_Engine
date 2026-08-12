// FBZZ Engine
// FzAssetFormat.hpp | fbzz::asset
// fz* バイナリアセットのオンディスクレイアウト定義
// WHY: Editor 側エクスポーターと Engine 側ローダーが同じ構造体を共有するため
//      Engine の public include/ に置く。実行時のデシリアライズコストを最小化するため
//      フォーマットは「ヘッダー + 連続したフラット配列」のみで構成し、
//      ポインタ・可変長フィールド・パディングを持たない。
#pragma once
#include <cstdint>

namespace fbzz::asset {

// ── .mesh ──────────────────────────────────────────────────────────────
// ヘッダー | 頂点配列 | インデックス配列
// 頂点型は flags で切り替え (静的: Vertex / スキン: SkinnedVertex)

constexpr uint32_t FZMESH_VERSION      = 1;
constexpr uint32_t FZMESH_FLAG_SKINNED = 1u << 0;

struct FzMeshHeader {
    char     magic[4];        // "FZMH"
    uint32_t version;
    uint32_t flags;           // FZMESH_FLAG_SKINNED など
    uint32_t vertexCount;
    uint32_t indexCount;
    float    boundsCenter[3];
    float    boundsRadius;
};
static_assert(sizeof(FzMeshHeader) == 36, "FzMeshHeader size mismatch");

// ── .skel ──────────────────────────────────────────────────────────────
// ヘッダー | FzSkeletonNodeData × nodeCount | FzBoneData × boneCount
// 各 SkeletonNode の children は childCount 個の int32_t が末尾に続く可変長エントリ

constexpr uint32_t FZSKEL_VERSION = 1;

struct FzSkelHeader {
    char     magic[4];        // "FZSK"
    uint32_t version;
    int32_t  rootNodeIndex;
    uint32_t nodeCount;
    uint32_t boneCount;
    float    rootInverse[16]; // row-major Matrix4
};
static_assert(sizeof(FzSkelHeader) == 84, "FzSkelHeader size mismatch");

// 1 SkeletonNode のシリアライズ形式
// children は直後に childCount × int32_t が続く
struct FzSkeletonNodeData {
    char     name[64];
    int32_t  parentIndex;
    int32_t  boneIndex;
    float    bindTranslation[3];
    float    bindRotation[4];     // x,y,z,w
    float    bindScale[3];
    float    localBindTransform[16];
    uint32_t childCount;
    // 直後: childCount × int32_t
};
static_assert(sizeof(FzSkeletonNodeData) == 180, "FzSkeletonNodeData size mismatch");

// 1 Bone のシリアライズ形式
struct FzBoneData {
    char     name[64];
    int32_t  nodeIndex;
    float    offsetMatrix[16];
    float    _pad;    // 64 バイトアライン
};
static_assert(sizeof(FzBoneData) == 136, "FzBoneData size mismatch");

// ── .anim ──────────────────────────────────────────────────────────────
// ヘッダー | FzAnimTrackHeader × trackCount | 各トラックの keyframe 配列

constexpr uint32_t FZANIM_VERSION = 3;

struct FzAnimHeader {
    char     magic[4];        // "FZAN"
    uint32_t version;
    char     name[128];
    double   durationTicks;
    double   ticksPerSecond;
    uint32_t trackCount;
    uint32_t _pad;
};
static_assert(sizeof(FzAnimHeader) == 160, "FzAnimHeader size mismatch");

// 1 トラックのヘッダー。直後に以下が続く:
//   positionCount × FzVectorKey
//   rotationCount × FzQuaternionKey
//   scaleCount    × FzVectorKey
struct FzAnimTrackHeader {
    char     nodeName[128];
    uint32_t positionCount;
    uint32_t rotationCount;
    uint32_t scaleCount;
    uint32_t _pad;
};
static_assert(sizeof(FzAnimTrackHeader) == 144, "FzAnimTrackHeader size mismatch");

struct FzVectorKey {
    double time;
    float  x, y, z;
    float  _pad;
};
static_assert(sizeof(FzVectorKey) == 24, "FzVectorKey size mismatch");

struct FzQuaternionKey {
    double time;
    float  x, y, z, w;
};
static_assert(sizeof(FzQuaternionKey) == 24, "FzQuaternionKey size mismatch");

// ─── IBL アセット descriptor (.ibl バイナリ) ─────────────────────────────────
//
// レイアウト: FzIblHeader のみ (参照先 DDS ファイルはすべて外部パス)
//
// 設計理由:
//   DDS は DirectX ネイティブフォーマットで再圧縮不要。バイナリに埋め込まず
//   外部参照方式を採用することでキャッシュ効率と差し替えのしやすさを両立する。
//   パスは .ibl ファイルと同一ディレクトリからの相対パスで格納する。

constexpr uint32_t FZIBL_VERSION = 1;

struct FzIblHeader {
    char     magic[6];               // "FZIBL\0"
    uint16_t version;                // FZIBL_VERSION
    uint32_t prefilteredMipCount;    // Prefiltered Cubemap の mip 数
    uint32_t _pad;
    char     envCubemapPath[256];    // Environment Cubemap DDS の相対パス
    char     irradiancePath[256];    // Irradiance Cubemap DDS の相対パス
    char     prefilteredPath[256];   // Prefiltered Cubemap DDS の相対パス
    char     brdfLutPath[256];       // BRDF LUT DDS の相対パス
};
// 6+2+4+4+256×4 = 1040 bytes
static_assert(sizeof(FzIblHeader) == 1040, "FzIblHeader size mismatch");

} // namespace fbzz::asset
