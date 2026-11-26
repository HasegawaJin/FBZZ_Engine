/// @file    RenderWaterInput.hpp
/// @brief   水面の形状・波・材質の解決済み入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <Graphics/Renderer/Geometry/SurfacePatchLod.hpp>
#include <array>
#include <memory>
#include <span>
namespace fbzz::renderer {
struct WaterStripIndices {
    ResourceHandle<BufferTag> buffer; ///< 三角形ストリップを描くインデックスバッファへの非所有ハンドル。
    uint32_t count = 0; ///< buffer のインデックス数。再開マーカーを含む。
};

struct RenderWaterPatch {
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer; ///< ローカル空間の水面パッチ頂点を持つ非所有ハンドル。
    renderer::ResourceHandle<renderer::BufferTag> indexBuffer; ///< 通常の三角形リストを描く非所有インデックスバッファ。
    uint32_t indexCount = 0; ///< indexBuffer に含まれるインデックス数。
    math::Vector3 aabbMin; ///< ローカル空間でパッチを覆う AABB の最小点 [m]。
    math::Vector3 aabbMax; ///< ローカル空間でパッチを覆う AABB の最大点 [m]。
    WaterStripIndices strip{}; ///< ストリップとして描けるパッチで使う代替インデックス列。
};

/// @note 最も細かい元形状の境界を格納する。子孫は後順のクラスター配列内で連続する。
struct WaterSurfaceCluster {
    math::Vector3 boundsMin; ///< 最も細かい元形状を囲うローカル最小点 [m]。
    math::Vector3 boundsMax; ///< 最も細かい元形状を囲うローカル最大点 [m]。
    float error = 0.0f; ///< このクラスターを簡略化したときの幾何誤差 [ローカル単位]。
    uint32_t firstDescendant = 0; ///< 後順に格納したクラスター配列内の最初の子孫の添字。
    uint32_t startIndex = 0; ///< 生成した共有インデックス列内の開始位置 [要素]。
    uint32_t indexCount = 0; ///< startIndex から描画するインデックス数。
};

/// @note Index ranges are relative to the generated index stream. Protected vertices retain their complete incident topology.
[[nodiscard]] FBZZ_GRAPHICS_API bool BuildWaterSurfaceClusters(std::span<const WaterVertex> vertices,
    std::span<const uint32_t> indices, std::span<const uint8_t> protectedVertices,
    std::vector<WaterSurfaceCluster>& outClusters, std::vector<uint32_t>& outIndices);

struct WaterWaveMargin {
    float horizontal = 0.0f; ///< 水平 XZ 方向の波変位を覆う境界余白 [m]。
    float vertical   = 0.0f; ///< 垂直 Y 方向の波変位を覆う境界余白 [m]。
};

/// @note local[m] = dot(row.xyz,world[m])+row.w。半径はlocal空間、親と符号付きscaleは行に解決済み。
struct WaterMediumExclusionInput {
    std::array<math::Vector4, 3> worldToLocalRows{}; ///< ワールド位置を除外箱のローカル位置に移すアフィン変換の行。
    math::Vector3 halfExtent{}; ///< 除外箱の中心から各面までのローカル距離 [m]。
};

/// @note Sphere position/radius [m] とRGB散乱係数[m^-1]、強度。寿命はScene側が更新する。
struct WaterMediumPlumeInput {
    math::Vector4 worldPositionRadius{}; ///< xyz はワールド空間の散乱球中心、w は半径 [m]。
    math::Vector4 scatteringDensity{}; ///< xyz は RGB 散乱係数 [m⁻¹]、w は散乱の強度倍率。
};

/// @note 一つの実効ownerだけを出力し、Sceneと物理への参照を持たない。
struct WaterMediumInput {
    uint64_t stableOwnerKey = 0; ///< ビューをまたいで同じ水域を識別するキー。
    int32_t priority = 0; ///< 水域が重なる位置で媒質の所有元を選ぶ優先度。
    math::Vector4 worldBoundsXZ{}; ///< ワールド空間の (最小 X, 最小 Z, 最大 X, 最大 Z) [m]。
    std::array<math::Vector4, 3> worldToLocalRows{}; ///< ワールド位置を水域ローカル位置へ移すアフィン変換の行。
    math::Vector4 absorptionPhaseG{}; ///< xyz は RGB 吸収係数 [m⁻¹]、w は散乱位相関数の異方性係数。
    math::Vector4 scatteringLight{}; ///< xyz は RGB 散乱係数 [m⁻¹]、w は照明による散乱の倍率。
    float surfaceY = 0.0f; ///< 基準水面のワールド Y [m]。
    float bottomY = 0.0f; ///< 水域下端のワールド Y [m]。
    float depthLimit = 0.0f; ///< 水面から鉛直下向きに測る深度の上限 [m]。
    float maxDistance = 120.0f; ///< 視線方向に水中光を積分する長さの上限 [m]。
    float transitionDepth = 0.15f; ///< 水面を横切ったとき水中効果を遷移させる深さの幅 [m]。
    float causticsIntensity = 1.0f; ///< コースティクス照明の強度倍率。
    float distortion = 0.001f; ///< 水中の画面空間歪みの強さ。
    int lightSteps = 48; ///< 水中散乱を積分するステップ数。
    bool opticalEnabled = false; ///< 吸収・散乱の水中光学を評価するかどうか。
    bool causticsEnabled = false; ///< 水中でコースティクス照明を評価するかどうか。
    bool analyticRectangle = false; ///< 矩形の横境界をアトラスからではなく解析形状から求められるかどうか。
    bool depthIsUniform = false; ///< 地形テクスチャを使う場合でも、物理深度が全域で depthLimit と等しいかどうか。
    math::Vector2 localHalfExtent{}; ///< 解析矩形の中心から端までのローカル XZ 距離 [m]。
    std::vector<WaterMediumExclusionInput> exclusions; ///< この水域から除く空気領域の入力列。
    std::vector<WaterMediumPlumeInput> plumes; ///< この水域内の局所散乱球の入力列。
};

struct RenderWaterInput {
    uint32_t layer = 0; ///< ビューのカリングマスクに照合するレイヤー番号。
    bool selected = false; ///< 水面を選択マスクへ描くかどうか。
    WaterCB constants{}; ///< 水面の変換、波、色のシェーダー定数。
    WaterEffectParams effects{}; ///< 水面パスの共通エフェクト設定。
    std::vector<WaterMaterialBlendTarget> zoneMaterials; ///< 水域の区画に割り当てる材質ブレンド対象。
    uint64_t horizonReceiverKey = 0; ///< 河口で接続する水平遠景水域の stableOwnerKey。0 は接続先なし。
    WaterMediumInput medium; ///< この水域の体積・吸収・散乱の解決済み入力。
    WaterWaveMargin margin; ///< 波が静止水面から外へ変位する分の境界余白。
    math::Vector3 aabbMin; ///< 水面全体のローカル AABB の最小点 [m]。
    math::Vector3 aabbMax; ///< 水面全体のローカル AABB の最大点 [m]。
    ResourceHandle<TextureTag> foam; ///< 泡の表面模様に使うテクスチャ。
    ResourceHandle<TextureTag> ripple; ///< さざ波を表現するテクスチャ。
    ResourceHandle<TextureTag> detailNoise; ///< 細かな水面法線の変化に使うノイズテクスチャ。
    ResourceHandle<TextureTag> velocityField; ///< 水面上の材質・泡を流すための速度場テクスチャ。
    ResourceHandle<ShaderTag> shader; ///< 材質に指定された水面シェーダーへの非所有ハンドル。
    ResourceHandle<ConstantBufferTag> appearanceParameters; ///< 抽出した材質が所有する b2 の外観定数バッファへの非所有ハンドル。
    std::array<ResourceHandle<TextureTag>, 8> appearanceTextures{}; ///< 外観定数の bindless 添字から読むテクスチャ。水面の共通スロットとは独立。
    std::vector<RenderWaterPatch> patches; ///< 個別に境界判定できる抽出済み水面パッチ。
    std::shared_ptr<const std::vector<WaterSurfaceCluster>> surfaceClusters; ///< スプライン・区画水面の不変な LOD クラスター列。
    float surfaceClusterErrorPixels = 1.0f; ///< 湖・川のクラスターを粗くできる投影誤差の上限 [px]。
    ResourceHandle<BufferTag> surfaceClusterIndices; ///< クラスターが共有するインデックス列への非所有ハンドル。
    ResourceHandle<BufferTag> surfaceClusterVertices; ///< 三角形ごとの区画材質を運ぶ専用頂点。不要なら単一パッチの頂点を使う。
    bool adaptivePatchLod = false; ///< 抽出元が所有する共有パッチから、ビュー固有の LOD を選ぶかどうか。
    bool patchMorphEnabled = false; ///< 選択マスクを含む全ビューの色・形状で共通の LOD 遷移を使うかどうか。
    SurfacePatchLodSettings patchLod; ///< 共有パッチの分割、LOD、遷移の設定。
    ResourceHandle<BufferTag> patchVertexBuffer; ///< ビューごとに選んだパッチで再利用する頂点バッファ。
    std::array<ResourceHandle<BufferTag>, 16> patchIndexBuffers{}; ///< 隣接辺の接続パターンごとのインデックスバッファ。
    std::array<uint32_t, 16> patchIndexCounts{}; ///< 各 patchIndexBuffers のインデックス数。
    bool farOceanEnabled = false; ///< 水面と水中アトラスをビュー範囲まで延長する水平遠景経路を使うかどうか。
    float farOceanCoverageScale = 1.05f; ///< 水平水域がビューを覆う範囲の倍率。
    float farOceanRippleFadeDistance = 4.0f; ///< 遠景との境界でさざ波を減衰させる距離 [m]。
    std::array<WaterStripIndices, 16> patchStrips{}; ///< 隣接辺の接続パターンごとのストリップ描画入力。
};
} // namespace fbzz::renderer
