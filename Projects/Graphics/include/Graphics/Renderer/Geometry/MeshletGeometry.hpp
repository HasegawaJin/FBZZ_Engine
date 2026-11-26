/// @file    MeshletGeometry.hpp
/// @brief   Static geometry meshlet packing and ResourceManager-owned cache.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>

#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class ResourceManager;
struct Vertex;
struct SkinnedVertex;

struct MeshletStreamPage {
    uint32_t depth = 0; ///< このページに属するクラスターの階層深度。
    std::vector<uint32_t> words; ///< ストリーミングで GPU へ送るパック済み頂点添字とプリミティブの 32 ビット列。
};

inline constexpr uint32_t CLUSTER_MIN_TRIANGLES = 1024u;

/// @note Import-owned packed geometry. The GPU blob is created only for a view using cluster LOD.
struct MeshletClusterData {
    std::vector<uint8_t> bytes; ///< ページ分割を使わない場合のメッシュレットとクラスター階層のパック済みデータ。
    uint32_t meshletCount = 0; ///< 階層内のメッシュレット記述子の総数。
    uint32_t nodeOffsetBytes = 0; ///< bytes 内のクラスター節点テーブル開始位置 [byte]。
    std::vector<uint8_t> streamMetadata; ///< ページ分割経路で常駐させる記述子と節点テーブルのパック済みデータ。
    std::vector<uint32_t> streamTable; ///< メッシュレット記述子の添字から、そのデータを含む streamPages の添字を引く表。
    std::vector<MeshletStreamPage> streamPages; ///< 深度ごとに分けたストリーミング用の形状ページ。
    uint32_t streamNodeOffsetBytes = 0; ///< streamMetadata 内のクラスター節点テーブル開始位置 [byte]。
    /// @note Maximum root error in bind-local units, used to request detail before it becomes visible.
    float streamRootError = 0.0f; ///< 可視になる前に詳細ページを要求するために保持するルートの最大誤差 [バインド姿勢のローカル単位]。
    math::Vector3 streamBoundsCenter{}; ///< ストリーミング対象を囲うローカル境界球の中心。
    float streamBoundsRadius = 0.0f; ///< ストリーミング対象を囲うローカル境界球の半径。
    float maximumSkinWeightError = 0.0f; ///< 頂点のスキン重み合計と 1 の差の絶対値の最大値。
};

/// @brief GPU meshlet descriptor shared with MeshletGeometry.hlsli.
struct MeshletDescriptor {
    float boundsMin[4]{}; ///< xyz はローカル AABB の最小点。w は経路に応じて法線コーンまたは階層節点の情報を格納。
    float boundsMax[4]{}; ///< xyz はローカル AABB の最大点。w は経路に応じて法線コーンまたはメッシュレットの情報を格納。
    uint32_t vertexOffsetBytes = 0; ///< メッシュレットの頂点添字列の開始位置 [byte]。
    uint32_t primitiveOffsetBytes = 0; ///< メッシュレットの三角形列の開始位置 [byte]。
    uint32_t vertexCount = 0; ///< メッシュレットが参照する頂点の数。
    uint32_t primitiveCount = 0; ///< メッシュレットが描くプリミティブの数。
};
static_assert(sizeof(MeshletDescriptor) == 48);

/// @brief Uploaded geometry blob, source draw parameters, and aggregate local-space bounds.
struct MeshletGeometry {
    ResourceHandle<BufferTag> blob{}; ///< ResourceManager が所有するパック済み GPU データの非所有ハンドル。
    uint32_t meshletCount = 0; ///< blob に含まれるメッシュレット記述子の件数。
    uint32_t vertexCount = 0; ///< 元メッシュの頂点数。
    uint32_t vertexStride = 0; ///< 元頂点バッファでの頂点レコード間隔 [byte]。
    uint32_t indexCount = 0; ///< 元メッシュのインデックス数。
    /// @note Aggregate position bounds use the source mesh's local space before world transform.
    std::array<float, 3> boundsMin{}; ///< 元メッシュのワールド変換前に求めたローカル AABB の最小点。
    std::array<float, 3> boundsMax{}; ///< 元メッシュのワールド変換前に求めたローカル AABB の最大点。
    uint32_t clusterNodeOffsetBytes = 0; ///< blob 内のクラスター節点テーブル開始位置 [byte]。
};

/// @brief Deterministic greedy partitioner for indexed triangle geometry.
class MeshletGeometryBuilder {
public:
    /// @return False when the mesh cannot be safely simplified or exceeds packing limits.
    [[nodiscard]] FBZZ_GRAPHICS_API static bool BuildClusters(std::span<const Vertex> vertices,
        std::span<const uint32_t> indices, MeshletClusterData& output);
    [[nodiscard]] FBZZ_GRAPHICS_API static bool BuildClusters(std::span<const SkinnedVertex> vertices,
        std::span<const uint32_t> indices, MeshletClusterData& output);

    /// @brief Builds contiguous meshlets with at most 64 vertices and 126 triangles.
    /// @param[in] vertexData 頂点のバイト列。各頂点の先頭に位置の xyz 成分が並ぶ。
    /// @param[in] vertexStride 頂点間のバイト数。12 以上かつ 4 の倍数。
    /// @param[in] indexData リトルエンディアンの uint32 三角形インデックス列。
    /// @param[in] indexCount インデックス数。0 より大きい 3 の倍数。
    /// @return False for invalid, non-finite, overflowing, or over-capacity input; outputs remain unchanged.
    /// @see https://github.com/microsoft/DirectXMesh/wiki/ComputeMeshlets «meshlet generation inputs and output streams»
    [[nodiscard]] FBZZ_GRAPHICS_API static bool Build(const uint8_t* vertexData, size_t vertexDataBytes,
                                    uint32_t vertexCount, uint32_t vertexStride,
                                    const uint8_t* indexData, size_t indexDataBytes,
                                    uint32_t indexCount,
                                    std::vector<uint8_t>& outBlob,
                                    uint32_t& outMeshletCount);
};

/// @brief ResourceManager-scoped cache for static meshlet blobs.
class MeshletGeometryCache {
public:
    /// @brief Reuses a blob while source handle generations and content versions match.
    /// @param[in,out] resources 入力バッファと生成したメッシュレットデータを所有する資源管理器。キャッシュミス時に読み込みや資源の確保に使う。
    /// @param[in] vertexBuffer Raw SRV として読み取れる頂点バッファ。各頂点の先頭に位置の xyz 成分が並ぶ。
    /// @param[in] indexBuffer CPU から読み取れる uint32 のインデックスバッファ。
    /// @return False when input metadata, CPU shadows, geometry, or GPU capacity is invalid; output remains unchanged.
    /// @pre The caller selects static geometry with no GPU deformation.
    /// @note Cache hits use mirrored metadata; misses synchronize through ResourceManager before reading CPU shadows.
    /// @note Cached blobs are bounded by count and byte budget; least-recently-used entries are evicted.
    [[nodiscard]] FBZZ_GRAPHICS_API bool Prepare(ResourceManager& resources,
                               ResourceHandle<BufferTag> vertexBuffer,
                               ResourceHandle<BufferTag> indexBuffer,
                               uint32_t vertexCount, uint32_t vertexStride,
                               uint32_t indexCount, MeshletGeometry& outGeometry,
                               const MeshletClusterData* clusters = nullptr, bool paged = false);

    /// @brief Caches the selected terrain topology with conservative geometric normal cones.
    /// @note The source terrain vertex buffer uses the 44-byte position/normal/tangent/UV layout.
    [[nodiscard]] FBZZ_GRAPHICS_API bool PrepareTerrain(ResourceManager& resources,
                               ResourceHandle<BufferTag> vertexBuffer,
                               ResourceHandle<BufferTag> indexBuffer,
                               uint32_t indexCount, MeshletGeometry& outGeometry);

    /// @brief Invalidates entries that use a released source or blob handle.
    FBZZ_GRAPHICS_API void Invalidate(ResourceManager& resources, ResourceHandle<BufferTag> buffer);

    /// @brief Releases every cached blob through its owning ResourceManager.
    FBZZ_GRAPHICS_API void Release(ResourceManager& resources);

private:
    [[nodiscard]] bool PrepareImpl(ResourceManager& resources, ResourceHandle<BufferTag> vertexBuffer,
                                  ResourceHandle<BufferTag> indexBuffer, uint32_t vertexCount,
                                  uint32_t vertexStride, uint32_t indexCount, bool terrainCones,
                                  MeshletGeometry& outGeometry, const MeshletClusterData* clusters, bool paged);

    struct InputKey {
        ResourceHandle<BufferTag> vertexBuffer{}; ///< 元形状の頂点バッファを世代込みで識別する非所有ハンドル。
        ResourceHandle<BufferTag> indexBuffer{}; ///< 元形状のインデックスバッファを世代込みで識別する非所有ハンドル。
        uint32_t vertexCount = 0; ///< キャッシュ候補を構築した元頂点の数。
        uint32_t vertexStride = 0; ///< キャッシュ候補を構築した元頂点のレコード間隔 [byte]。
        uint32_t indexCount = 0; ///< キャッシュ候補を構築した元インデックスの数。
        bool terrainCones = false; ///< 地形の法線コーンを格納するデータ形式を使うかどうか。
        bool clustered = false; ///< クラスター階層を含む経路かどうか。
        bool paged = false; ///< ストリーミング用のページ形式を使うかどうか。

        bool operator==(const InputKey&) const = default;
    };

    struct InputKeyHash {
        [[nodiscard]] FBZZ_GRAPHICS_API size_t operator()(const InputKey& key) const;
    };

    struct Entry {
        uint64_t vertexVersion = 0; ///< 項目の構築時に使った頂点バッファの内容版。
        uint64_t indexVersion = 0; ///< 項目の構築時に使ったインデックスバッファの内容版。
        uint64_t lastUseStamp = 0; ///< キャッシュの退役順に使う最終参照番号。
        size_t blobBytes = 0; ///< キャッシュ予算に計上する GPU データ容量 [byte]。
        MeshletGeometry geometry{}; ///< 頂点・インデックスの内容版が一致するときに再利用するメッシュレット情報。
        bool rejected = false; ///< 無効な入力を同じ内容版で再構築しないための拒否結果。
    };

    std::unordered_map<InputKey, Entry, InputKeyHash> m_entries; ///< 入力ハンドルと形式の組でメッシュレット情報を引くキャッシュ。
    mutable std::mutex m_mutex; ///< キャッシュの検索、更新、退役処理を同期するロック。
    uint64_t m_ownerInstanceId = 0; ///< キャッシュの GPU データを所有する ResourceManager の識別番号。
    uint64_t m_resetVersion = 0; ///< 各キャッシュ項目を構築した資源のリセット世代。
    uint64_t m_useStamp = 0; ///< キャッシュ項目の参照順を示す内部の単調増加番号。
    size_t m_cachedBytes = 0; ///< キャッシュに保持中の GPU データの容量合計 [byte]。
    bool m_releasing = false; ///< 資源の一括解放中に再入する無効化処理を抑止するフラグ。
};

} // namespace fbzz::renderer
