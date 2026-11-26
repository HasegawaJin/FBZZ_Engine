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
    uint32_t objectIndex = 0; ///< この項目の姿勢を引く RenderScene::Objects() 内の添字。
    uint32_t materialSlot = 0; ///< 元メッシュの材質スロット番号。
    uint32_t sourceSubmesh = 0; ///< 元メッシュ内のサブメッシュ番号。
    ResourceHandle<BufferTag> vertexBuffer; ///< ResourceManager が所有する通常描画用の頂点バッファ。
    ResourceHandle<BufferTag> indexBuffer; ///< ResourceManager が所有するインデックスバッファ。
    ResourceHandle<BufferTag> skinningVertexBuffer; ///< VS スキニングの入力。モーフがある場合はインスタンス専用の頂点列。
    ResourceHandle<BufferTag> deformedVertexBuffer; ///< 当該フレームに変形済みの静的レイアウト。VS スキニングの入力には使わない。
    std::shared_ptr<PreparedSkinningTicket> preparedSkinning; ///< 元入力と GPU 資源を保持し、公開済みスナップショットからも変形出力を参照できるようにする。
    uint32_t preparedSkinningWrite = UINT32_MAX; ///< チケット内の出力行の添字。UINT32_MAX は未割り当て。
    uint32_t indexCount = 0; ///< indexBuffer の描画対象インデックス数。
    uint32_t vertexCount = 0; ///< この項目の描画対象頂点数。
    /// @note 内容版 0 は未解決または GPU 変形専用。バッファの世代と独立して Update を識別する。
    uint64_t vertexContentVersion = 0; ///< 通常頂点バッファの内容版。
    uint64_t indexContentVersion = 0; ///< インデックスバッファの内容版。
    uint64_t deformedContentVersion = 0; ///< 変形済み頂点バッファの内容版。
    uint32_t vertexStride = 0; ///< 頂点レコードの間隔 [byte]。
    uint32_t vertexPositionOffset = 0; ///< 各頂点の先頭から位置成分までの距離 [byte]。
    RenderMaterial material; ///< この項目の材質スロットに解決された描画材質。
    RenderMaterial forwardMaterial; ///< Surface 材質をスキンドに誤指定したときの Forward 用代替。
    bool visible = true; ///< 抽出時に設定された描画項目全体の可視性。
    bool slotVisible = true; ///< この材質スロットの可視性。
    bool reliableOccluder = false; ///< 遮蔽物の候補にするための形状密度の指標。被覆判定には内接半径と材質を別途使う。
    math::Vector3 boundsCenter{}; ///< ワールド空間の境界球中心 [m]。
    float boundsRadius = 0.0f; ///< ワールド空間で項目を覆う境界球半径 [m]。
    float occluderRadius = 0.0f; ///< 実形状に内接するワールド球の半径 [m]。最小軸スケールで縮め、カリング余白は足さない。
    std::shared_ptr<const MeshletClusterData> clusterLod; ///< ビューごとのメッシュレット LOD 選択に使う不変の階層データ。
};

/// @brief 当該抽出フレーム用のボーン行列パレット。
/// @note GPU ハンドルは非所有。オフセットは行列要素数。
struct RenderGpuBonePalette {
    ResourceHandle<StructuredBufferTag> current; ///< 当該フレームのボーン行列を読むバッファ。
    ResourceHandle<StructuredBufferTag> previous; ///< 前フレームのボーン行列を読むバッファ。
    uint32_t currentOffset = 0; ///< current 内のこの物体の先頭 [行列数]。
    uint32_t previousOffset = 0; ///< previous 内のこの物体の先頭 [行列数]。
};
/// @brief 一つの描画物体の姿勢と、その姿勢を使う描画項目の範囲。
struct RenderObject {
    RenderGpuBonePalette gpuBones; ///< GPU バッファから当該フレームと前フレームの姿勢を読む参照。
    /// @note sceneGeneration と組にした Entity ID。vector 添字や入力への参照は snapshotSerial を跨いで持ち越さない。
    uint32_t sourceIndex = 0; ///< 抽出元 Entity ID の添字部。
    uint32_t sourceGeneration = 0; ///< 同じ添字を再利用した別 Entity と区別する世代部。
    /// @note 対応する RenderScene::items の半開区間は [firstItem, firstItem + itemCount)。
    uint32_t firstItem = 0; ///< この物体の最初の描画項目を指す RenderScene::items 内の添字。
    uint32_t itemCount = 0; ///< firstItem から続く描画項目数。
    uint32_t layer = 0; ///< ビューのカリングマスクに照合するレイヤー番号。
    uint32_t waterfallFarIndexStart = 0; ///< 生成した滝の共有インデックスバッファ内の遠景区間の先頭。0 は遠景なし。
    bool skinned = false; ///< ボーン姿勢に従って頂点を変形するかどうか。
    bool castShadows = true; ///< 影の描画候補に含めるかどうか。
    bool lodVisible = true; ///< ラスター描画で選択された LOD の可視性。
    bool colorEligible = false; ///< 色パスの描画候補として抽出されたかどうか。
    bool selected = false; ///< 抽出元でこの物体が選択されているかどうか。
    float lodDither = 0.0f; ///< ラスター描画での LOD 遷移のディザ閾値。0 は遷移なし。
    math::Matrix4 world = math::Matrix4::Identity(); ///< 抽出時のモデルローカル空間からワールド空間への変換。
    math::Matrix4 worldInvTranspose = math::Matrix4::Identity(); ///< ローカル法線をワールド空間へ移す逆転置行列。
    math::Matrix4 previousWorld = math::Matrix4::Identity(); ///< 速度パスが使う前フレームのワールド変換。
    math::Vector3 sortPosition{}; ///< Forward・Deferred の並べ替えに使う位置。通常はローカル空間、滝はワールド空間。
    math::Vector3 boundsCenter{}; ///< 物体を覆うワールド境界球の中心 [m]。
    float boundsRadius = 0.0f; ///< 物体を覆うワールド境界球の半径 [m]。
    float leafWindBoundsPadding = 0.0f; ///< 葉の風変形による境界の余白 [m]。-1 は材質の未解決。
    float skinClusterDisplacement = -1.0f; ///< バインド姿勢からの頂点最大変位 [m]。負値は未取得。
    ResourceHandle<ConstantBufferTag> skinningPalette; ///< 現在姿勢のボーン行列定数バッファ。
    ResourceHandle<ConstantBufferTag> previousSkinningPalette; ///< 前回姿勢のボーン行列定数バッファ。
    bool previousSkinningValid = false; ///< 速度計算に使える前回姿勢があるかどうか。
    bool rayLodSelectionRequired = false; ///< レイ用に LODGroup の形状をカメラから独立して選ぶ必要があるかどうか。
    bool rayVisible = true; ///< ラスターの LOD 選択・遷移・カリングに依存しない、レイ用 LOD0 の可視性。
    bool shadowBakeStatic = false; ///< ホストが不変の内容版と変換を検証でき、静的影を焼く候補にできるかどうか。
};

struct RenderInstancePart {
    RenderMeshItem mesh; ///< 配置セットの各個体で再利用するメッシュと材質。
    uint32_t lod = 0; ///< 部品が属する RenderInstanceInput の LOD 番号。
};
/// @note 各配置元の 128m ワールド XZ セル内で、高精細描画に使う安定順の個体数。0 は上限なし。
struct InstanceVegetationBudget {
    bool enabled = false; ///< セルごとの高精細個体数制限を使うかどうか。
    uint32_t drawDetailPerCell = 0; ///< セル内でカラー描画が高精細に扱う個体数の上限。
    uint32_t nearDetailPerCell = 0; ///< セル内で近景の詳細 LOD を許可する個体数の上限。
    uint32_t shadowDetailPerCell = 0; ///< セル内で影の詳細 LOD を許可する個体数の上限。
};
/// @brief 配置セットと LOD 別の解決済み描画部品。
/// @note 配置データは不変の版を共有し、parts の GPU ハンドルは非所有。
struct RenderInstanceInput {
    std::array<uint64_t, 2> identity{}; ///< ビュー間の常駐配置キャッシュで同じ配置元を照合する識別子。
    uint32_t layer = 0; ///< ビューのカリングマスクに照合するレイヤー番号。
    bool selected = false; ///< この配置セットを選択マスクに含めるかどうか。
    bool castShadows = true; ///< この配置セットで影を描くかどうか。
    bool selectionOnly = false; ///< 選択表示にだけ使う配置かどうか。
    float maxDistance = 0; ///< ビューからの追加カリング距離上限 [m]。0 は追加制限なし。
    math::Matrix4 world = math::Matrix4::Identity(); ///< 配置セットのローカル座標をワールドへ移す変換。
    math::Vector3 localBoundsCenter{}; ///< 各インスタンスの全 LOD を覆うローカル境界球の中心。
    float localBoundsRadius = 0; ///< ローカル単位で全 LOD の形状と葉の最大風変位を包む半径。
    std::array<float, 4> lodHeights{}; ///< 各 LOD の選択に使う画面高さ比の閾値。
    uint32_t lodCount = 0; ///< 有効な LOD 数。lodHeights の先頭からこの件数を読む。
    std::shared_ptr<const InstanceSetData> data; ///< 複数ビューが共有する不変のインスタンス配置版。
    std::vector<RenderInstancePart> parts; ///< LOD 番号を持つ解決済みの描画部品列。
    InstanceVegetationBudget vegetationBudget; ///< 画面と影で処理する高精細な植生のセル単位上限。
};

} // namespace fbzz::renderer
