/// @file    RayGeometryCache.hpp
/// @brief   静的 BLAS と対象集合別 TLAS・ヒット表を同じ版で公開する。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/RayTracing/Scene/RayScene.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>

namespace fbzz::renderer {

/// @note LAYOUT: RayDebug.cs.hlsl の RayHitRecord と一致させる。
struct RayHitRecord {
    uint32_t vertexSrv = UINT32_MAX; ///< 元の頂点列を読む bindless SRV 添字。未割り当ては `UINT32_MAX`。
    uint32_t indexSrv = UINT32_MAX; ///< 元のインデックス列を読む bindless SRV 添字。非インデックス形状は `UINT32_MAX`。
    uint32_t vertexStride = 0; ///< 元頂点レコードの間隔 [byte]。
    uint32_t positionOffset = 0; ///< 頂点レコード内のローカル位置までのオフセット [byte]。
    uint32_t firstVertex = 0; ///< 元頂点列でこの形状が始まる頂点添字。
    uint32_t firstIndex = 0; ///< 元インデックス列でこの形状が始まる添字。
    uint32_t indexCount = 0; ///< この形状のインデックス数。
    uint32_t vertexCount = 0; ///< この形状が参照できる頂点の数。
    uint32_t objectIndex = 0; ///< ヒットしたオブジェクトの Entity ID の添字部。
    uint32_t objectGeneration = 0; ///< ヒットしたオブジェクトの Entity ID の世代部。
    uint32_t sceneGenerationLow = 0; ///< ヒット元シーンを識別する世代番号の下位 32 bit。
    uint32_t sceneGenerationHigh = 0; ///< ヒット元シーンを識別する世代番号の上位 32 bit。
    bool operator==(const RayHitRecord&) const = default;
};
static_assert(sizeof(RayHitRecord) == 48);

/// @note 非所有の公開結果。次の Prepare / Release まで CPU で参照し、GPU 退役は ResourceManager が保護する。
struct RaySceneGpu {
    ResourceHandle<AccelerationStructureTag> topLevel; ///< 収集済みインスタンスを交差判定する構築済み TLAS の非所有ハンドル。
    ResourceHandle<StructuredBufferTag> hitRecords; ///< 密な InstanceID でヒット情報を参照する GPU 表。
    ResourceHandle<StructuredBufferTag> surfaceMaterials; ///< 密な InstanceID で表面材質を参照する GPU 表。
    std::vector<ResourceHandle<BufferTag>> readBuffers; ///< レイ交差の結果から参照する元の頂点・添字バッファ。
    std::vector<ResourceHandle<TextureTag>> readTextures; ///< レイの表面材質評価で参照するテクスチャの列。
    uint32_t instanceCount = 0; ///< 公開した TLAS とレコード表のインスタンス数。
    uint32_t builtBottomLevels = 0; ///< 今回の準備で新たに構築を記録した BLAS 数。
    bool builtTopLevel = false; ///< 今回の準備で新たに TLAS 構築を記録したか。
    bool ready = false; ///< この対象集合の TLAS と GPU 表を同じ版で参照できるか。
    /// @note True only for the submitted Hybrid candidate/culling policy; a manually assembled scene defaults to conservative queries.
    bool hybridCandidatePolicy = false; ///< Hybrid 固有のレイ候補・裏面カリングの契約で公開したか。
};

/// @note ResourceManager::Rendering の共有状態に置く。異なる Manager / device へ持ち越さない。
/// @note 版の不一致は旧 TLAS へ復帰せず失敗する。静的 geometry は full key の等値で共有する。
class RayGeometryCache {
public:
    /// @pre フレーム内の DIRECT queue で、compute batch / 非同期区間の外から呼ぶ。
    /// @note 需要のあるビューだけが呼ぶ。新規 AS は共有準備 graph を実行してから公開する。
    /// @note TLAS inputs and each immutable table are compared independently; failed preparation never publishes prior handles as current.
    [[nodiscard]] FBZZ_GRAPHICS_API RaySceneGpu Prepare(const RayScene& scene, RenderPassContext& context);
    FBZZ_GRAPHICS_API void Release(ResourceManager& resources);
    void Trim(ResourceManager& resources, uint64_t frameStamp) { Prune(resources, frameStamp); }
    [[nodiscard]] uint32_t BottomLevelCount() const { return static_cast<uint32_t>(m_geometry.size()); }
private:
    struct GeometryEntry {
        RayGeometryKey key; ///< 既存 BLAS を再利用できる頂点・添字・被覆入力を照合するキー。
        ResourceHandle<AccelerationStructureTag> handle; ///< このキャッシュが退役を管理する BLAS のハンドル。
        uint64_t lastUse = 0; ///< この BLAS を最後に使用した描画フレーム番号。
    };
    struct SceneEntry {
        uint64_t sceneGeneration = 0; ///< この TLAS を構築したシーンの世代。
        uint32_t layerMask = 0; ///< この TLAS に収集した Engine レイヤーのビット集合。
        std::vector<RayInstance> instances; ///< TLAS の再利用可否を照合するインスタンス入力。
        std::vector<RayHitRecord> records; ///< ヒット表の内容更新を判定する CPU 側レコード。
        std::vector<RaySurfaceRecord> surfaces; ///< 材質表の内容更新を判定する CPU 側レコード。
        RaySceneGpu gpu; ///< このキャッシュが保持する TLAS と表の GPU 公開結果。
        uint64_t lastUse = 0; ///< この TLAS と表を最後に使用した描画フレーム番号。
    };
    FBZZ_GRAPHICS_API void Prune(ResourceManager& resources, uint64_t frameStamp);
    std::vector<GeometryEntry> m_geometry; ///< 形状キーごとに再利用・退役する BLAS のキャッシュ。
    std::vector<SceneEntry> m_scenes; ///< 対象シーンとレイヤー集合ごとに保持する TLAS・表のキャッシュ。
    RenderPipeline m_preparation; ///< BLAS と TLAS の構築を直接キューへ記録する準備パイプライン。
};

} // namespace fbzz::renderer
