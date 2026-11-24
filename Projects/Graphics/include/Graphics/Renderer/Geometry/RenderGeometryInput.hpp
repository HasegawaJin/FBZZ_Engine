/// @file    RenderGeometryInput.hpp
/// @brief   メッシュ・姿勢・インスタンス配置の描画入力。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <Graphics/Scene/RenderInstanceInput.hpp>
#include <Graphics/Renderer/Materials/RenderMaterial.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fbzz::renderer {

class PreparedSkinningTicket;
struct MeshletClusterData;

/// @brief 形状バッファとこのビューの材質情報をまとめた描画単位。
struct RenderMeshItem {
    /// @note この項目の姿勢を引くための RenderScene::Objects() 内の添字。
    uint32_t objectIndex = 0;
    uint32_t materialSlot = 0;
    uint32_t sourceSubmesh = 0;
    /// @note 頂点・インデックスのバッファは ResourceManager が所有する。
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    /// @note VS スキニング入力。モーフがある場合はインスタンス専用の頂点列。
    ResourceHandle<BufferTag> skinningVertexBuffer;
    /// @note 当該フレームに計算済みの静的レイアウト。VS スキニングへ渡してはならない。
    ResourceHandle<BufferTag> deformedVertexBuffer;
    /// @note 出力 ticket は元入力と GPU 資源の lease を保持し、公開済みスナップショットからも参照できる。
    std::shared_ptr<PreparedSkinningTicket> preparedSkinning;
    /// @note ticket 内の出力行。UINT32_MAX は未割り当て。
    uint32_t preparedSkinningWrite = UINT32_MAX;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    /// @note 内容版 0 は未解決または GPU 変形専用。バッファの世代と独立して Update を識別する。
    uint64_t vertexContentVersion = 0;
    uint64_t indexContentVersion = 0;
    uint64_t deformedContentVersion = 0;
    /// @note 頂点ストライドと頂点先頭から位置までのオフセットはバイト単位。
    uint32_t vertexStride = 0;
    uint32_t vertexPositionOffset = 0;
    RenderMaterial material;
    /// @note Surface 材質をスキンドに誤指定したときの Forward 用代替。
    RenderMaterial forwardMaterial;
    bool visible = true;
    bool slotVisible = true;
    /// @note 形状密度の粗い指標。遮蔽被覆の証明には occluderRadius と材質契約を使う。
    bool reliableOccluder = false;
    /// @note ワールド空間の境界球 [m]。
    math::Vector3 boundsCenter{};
    float boundsRadius = 0.0f;
    /// @note 実形状内部のワールド球。最小軸スケールで縮小し、カリング余白は加えない。
    float occluderRadius = 0.0f;
    std::shared_ptr<const MeshletClusterData> clusterLod;
};

/// @brief 当該抽出フレーム用のボーン行列パレット。
/// @note GPU ハンドルは非所有。オフセットは行列要素数。
struct RenderGpuBonePalette {
    ResourceHandle<StructuredBufferTag> current, previous;
    uint32_t currentOffset = 0, previousOffset = 0;
};
/// @brief 一つの描画物体の姿勢と、その姿勢を使う描画項目の範囲。
struct RenderObject {
    RenderGpuBonePalette gpuBones;
    /// @note sceneGeneration と組にした Entity ID。vector 添字や入力への参照は snapshotSerial を跨いで持ち越さない。
    uint32_t sourceIndex = 0;
    uint32_t sourceGeneration = 0;
    /// @note 対応する RenderScene::items の半開区間は [firstItem, firstItem + itemCount)。
    uint32_t firstItem = 0;
    uint32_t itemCount = 0;
    uint32_t layer = 0;
    /// @note 生成した滝の遠景区間の先頭インデックス。0 は遠景区間なし。近景と遠景は同じバッファを参照する。
    uint32_t waterfallFarIndexStart = 0;
    bool skinned = false;
    bool castShadows = true;
    bool lodVisible = true;
    bool colorEligible = false;
    bool selected = false;
    float lodDither = 0.0f;
    /// @note 抽出時点のモデルローカル空間からワールド空間への変換。
    math::Matrix4 world = math::Matrix4::Identity();
    math::Matrix4 worldInvTranspose = math::Matrix4::Identity();
    math::Matrix4 previousWorld = math::Matrix4::Identity();
    /// @note 既存 Forward の並べ替え基準を保つローカル位置。描画行列は world を使う。
    math::Vector3 sortPosition{};
    /// @note ワールド空間の境界球 [m]。
    math::Vector3 boundsCenter{};
    float boundsRadius = 0.0f;
    /// @note 葉の風変形による境界の余白 [m]。-1 は抽出時に材質が未解決。
    float leafWindBoundsPadding = 0.0f;
    /// @note バインド姿勢からの頂点の最大変位 [m]。負値は未取得。
    float skinClusterDisplacement = -1.0f;
    ResourceHandle<ConstantBufferTag> skinningPalette;
    ResourceHandle<ConstantBufferTag> previousSkinningPalette;
    bool previousSkinningValid = false;
    /// @note LODGroup はレイ用にカメラ非依存の形状選択が必要。lodVisible は描画用の判定。
    bool rayLodSelectionRequired = false;
    /// @note レイは LOD0 を固定使用し、Raster の選択・crossfade・cull とは独立する。
    bool rayVisible = true;
    /// @note ホストが静的な版と変換を検証できる場合だけベイク候補にする。
    bool shadowBakeStatic = false;
};

struct RenderInstancePart {
    RenderMeshItem mesh;
    uint32_t lod=0;
};
/// @note 各配置元の 128m ワールド XZ セル内で、高精細描画に使う安定順の個体数。0 は上限なし。
struct InstanceVegetationBudget {
    bool enabled = false;
    uint32_t drawDetailPerCell = 0;
    uint32_t nearDetailPerCell = 0;
    uint32_t shadowDetailPerCell = 0;
};
/// @brief 配置セットと LOD 別の解決済み描画部品。
/// @note 配置データは不変の版を共有し、parts の GPU ハンドルは非所有。
struct RenderInstanceInput {
    std::array<uint64_t,2> identity{};
    uint32_t layer=0;
    bool selected=false, castShadows=true;
    bool selectionOnly=false;
    /// @note ビュー側の距離制限に重ねる上限 [m]。0 は追加上限なし。
    float maxDistance=0;
    math::Matrix4 world=math::Matrix4::Identity();
    math::Vector3 localBoundsCenter{};
    /// @note 配置セットのローカル単位で全 LOD の形状と葉の最大風変位を包む。
    float localBoundsRadius=0;
    std::array<float,4> lodHeights{};
    uint32_t lodCount=0;
    std::shared_ptr<const InstanceSetData> data;
    std::vector<RenderInstancePart> parts;
    InstanceVegetationBudget vegetationBudget;
};

} // namespace fbzz::renderer
