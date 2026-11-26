/// @file    FzModelFormat.hpp
/// @brief   .fzasset バイナリのオンディスクレイアウト定義。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// メッシュ複数 + LOD + スケルトンを 1 ファイルに統合する。
/// 旧 FzAssetFormat.hpp の FZMH/FZSK 部分を置き換える。
///
/// レイアウト:
/// FzModelHeader
/// materialSlotNames: materialSlotCount × 64 バイト (null 終端)
/// LOD[0..lodCount-1]:
/// FzLodHeader
/// FzSubmeshHeader[0..submeshCount-1]
/// 頂点データ (Vertex or SkinnedVertex)
/// インデックスデータ (uint32 × indexCount)
/// スケルトン (SKINNED フラグ時):
/// FzSkelHeader
/// FzSkeletonNodeData[] (各ノードの childCount × int32 が直後に続く)
/// FzBoneData[]
/// ノード階層 (v4 以降・NODES フラグ時):
/// FzModelNodeChunkHeader
/// FzModelNodeData[] (各ノードの meshCount × uint32 と childCount × int32 が直後に続く)
#pragma once
#include <Engine/Format/FzAssetFormat.hpp>
#include <cstdint>

namespace fbzz::asset {

/// v4: 末尾に DCC のノード階層チャンク (FZND) を追加。
/// @note 末尾へ足すと既存の全オフセットに触れず拡張できる。読み手は LOD とスケルトンを
///       読み終えた後、フラグを見てから続きを読むだけでよい。
/// v5: 静的サブメッシュの頂点に頂点カラー (float4) が付いた (FzVertexV1 → renderer::Vertex)。
///     スキンサブメッシュの SkinnedVertex は変わらない。
constexpr uint32_t FZMODEL_VERSION      = 5;
/// 頂点カラーを持たない静的頂点で書かれた版。読み込み時の分岐に使う。
constexpr uint32_t FZMODEL_VERSION_PRE_VERTEX_COLOR = 4;
constexpr uint32_t FZMODEL_FLAG_SKINNED = 1u << 0;
constexpr uint32_t FZMODEL_FLAG_LOD     = 1u << 1;
/// ノード階層チャンクが存在する。メッシュを持たないモデルでは立たない。
constexpr uint32_t FZMODEL_FLAG_NODES   = 1u << 2;
/// ノードの変換が既に頂点へ焼き込まれている。
/// @note 静的 (PreTransform) もスキンド (ボーンパレット経由) も既に頂点へベイクしているため、
///       保存された TRS を無条件に GameObject の Transform へ入れると二重変換になる。配置側が
///       「入れてよいか」をこのフラグで判断すること。無視するとモデルが原点へ潰れる/二重に回る。
constexpr uint32_t FZMODEL_FLAG_NODE_TRANSFORMS_BAKED = 1u << 3;
constexpr uint32_t FZMODEL_SLOT_NAME_LEN = 64;
constexpr uint32_t FZMODEL_NODE_NAME_LEN = 64;

struct FzModelHeader {
    char     magic[4]; ///< モデルファイルの形式を照合する 4 バイトの識別子 FZMD。
    uint32_t version; ///< フィールド配置を選ぶモデルファイルの版番号。
    uint32_t flags; ///< スケルトンなどの追加データの有無を表す FZMODEL_FLAG_* のビット集合。
    uint32_t lodCount; ///< モデルファイル内の LOD レコード数。
    uint32_t materialSlotCount; ///< モデル全体で参照できる材質スロット数。
    float    boundsCenter[3]; ///< 対象メッシュ集合を覆う境界球のモデル空間中心 XYZ。
    float    boundsRadius; ///< 対象メッシュ集合を覆う境界球のモデル空間半径。
    uint32_t _pad[3]; ///< 対応するバイナリレコードのバイト配置を保つ予約領域。
};
static_assert(sizeof(FzModelHeader) == 48, "FzModelHeader size mismatch");

struct FzLodHeader {
    float    screenSizeThreshold; ///< このLODを使う画面占有率の下限 (0.0 = 常時使用)
    uint32_t submeshCount; ///< この LOD に後続するサブメッシュの数。
};
static_assert(sizeof(FzLodHeader) == 8, "FzLodHeader size mismatch");

struct FzSubmeshHeader {
    uint32_t materialSlotIndex; ///< このサブメッシュが使用するモデルの材質スロット番号。
    uint32_t vertexFormat; ///< 頂点レコードの形式番号。0 は静的頂点、1 はスキンド頂点。
    uint32_t vertexCount; ///< 対応するレコードに続く頂点データの要素数。
    uint32_t indexCount; ///< このサブメッシュに後続するインデックスの要素数。
    float    boundsCenter[3]; ///< 対象メッシュ集合を覆う境界球のモデル空間中心 XYZ。
    float    boundsRadius; ///< 対象メッシュ集合を覆う境界球のモデル空間半径。
};
static_assert(sizeof(FzSubmeshHeader) == 32, "FzSubmeshHeader size mismatch");

/// v3 では、インポート元の aiMesh::mName をサブメッシュごとに保持する。
/// @note Scene 配置時に Mesh_0 のようなエンジン都合の名前へ置き換えず、
///       FBX のメッシュ名をそのまま GameObject 名へ反映するため。
struct FzSubmeshExtensionV3 {
    uint32_t morphTargetCount; ///< このサブメッシュに後続するモーフターゲット数。
    char     name[128]; ///< サブメッシュ名を格納する固定長バイト欄。
};

struct FzMorphTargetHeader {
    char name[128]; ///< モーフをアニメーション側から照合する固定長の名前欄。
    uint32_t vertexCount; ///< このモーフに格納した頂点差分の要素数。
};

struct FzMorphDelta {
    float position[3]; ///< 基底頂点からの位置差分 XYZ。
    float normal[3]; ///< 基底頂点からの法線差分 XYZ。
    float tangent[3]; ///< 基底頂点からの接線差分 XYZ。
};
static_assert(sizeof(FzMorphDelta) == 36, "FzMorphDelta size mismatch");

/// DCC のノード 1 個 = 配置時の 1 GameObject。ノード内のマテリアル分割は
/// meshIndices の複数要素 (= Renderer の submesh 列) として表現する。
struct FzModelNodeChunkHeader {
    char     magic[4]; ///< ノードチャンクを照合する 4 バイトの識別子 FZND。
    uint32_t nodeCount; ///< ノードチャンクに格納するモデルノード数。
    int32_t  rootNodeIndex; ///< モデルノードレコードの先頭のルート添字。未設定時は -1。
    uint32_t _pad; ///< 対応するバイナリレコードのバイト配置を保つ予約領域。
};
static_assert(sizeof(FzModelNodeChunkHeader) == 16, "FzModelNodeChunkHeader size mismatch");

/// 1 ノードのシリアライズ形式。
/// 直後に meshCount × uint32_t (lods[0].submeshes の添字)、
/// 続いて childCount × int32_t が並ぶ。
struct FzModelNodeData {
    char     name[FZMODEL_NODE_NAME_LEN]; ///< このモデルノードを照合する固定長の名前欄。
    int32_t  parentIndex; ///< モデルノード配列内での親の添字。ルートは -1。
    float    localTranslation[3]; ///< 親ノードを基準としたローカル位置 XYZ。
    float    localRotation[4]; ///< 親ノードを基準とした回転。クォータニオンの x、y、z、w 順。
    float    localScale[3]; ///< 親ノードを基準としたローカル軸別倍率 XYZ。
    uint32_t meshCount; ///< ノードレコードの後ろに並ぶメッシュ添字の数。
    uint32_t childCount; ///< ノードレコードの後ろに並ぶ直下の子ノード添字の数。
};
static_assert(sizeof(FzModelNodeData) == 116, "FzModelNodeData size mismatch");

/// FzSkelHeader / FzSkeletonNodeData / FzBoneData / FZSKEL_VERSION は
/// FzAssetFormat.hpp で定義済み (上記 include 経由で参照可)

} // namespace fbzz::asset
