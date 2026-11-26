/// @file    SurfacePatchLod.hpp
/// @brief   有限の連続面を共有格子で描くためのビュー別パッチ LOD と境界インデックス。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Graphics/Api.hpp>

#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fbzz::math { struct Frustum; }

namespace fbzz::renderer {

enum class SurfacePatchEdge : uint8_t {
    MIN_X = 1u << 0,
    MAX_X = 1u << 1,
    MIN_Z = 1u << 2,
    MAX_Z = 1u << 3,
};

struct SurfacePatchLodSettings {
    uint32_t cellsPerPatch = 16; ///< パッチの各辺に並ぶ共有格子のセル数。2〜256 の二の冪。
    uint32_t maxDepth = 8; ///< 基準矩形を 0 とした四分木の分割深度上限。0〜10。
    uint32_t maxPatches = 4096; ///< 視錐台外も含む葉パッチの予算。1〜16384。
    bool morphEnabled = false; ///< パッチ境界と同じ高さ場を保つ画面密度ベースの遷移を有効にする。
    float morphTransitionWidth = 0.25f; ///< 親レベルの投影要求が 1 を越えてから詳細へ戻るまでの正規化幅。
    float targetCellPixels = 24.0f; ///< 分割の基準とする画面への投影セル幅 [px]。
    float nearDistance = 64.0f; ///< 近景の追加分割を効かせるカメラからの距離 [m]。0 で無効。
    float nearCellSize = 2.0f; ///< 近景で目標とするワールド空間のセル幅 [m]。
};

struct SurfacePatchSurface {
    math::Vector2 localMin = { -50.0f, -50.0f }; ///< 水平変位を含まない基準矩形のローカル XZ 最小点。
    math::Vector2 localMax = { 50.0f, 50.0f }; ///< 水平変位を含まない基準矩形のローカル XZ 最大点。
    math::Matrix4 worldMatrix = math::Matrix4::Identity(); ///< 基準矩形のローカル座標からワールド座標へのアフィン変換。
    float minHeight = 0.0f; ///< 変位の全範囲を含むローカル Y の下限。
    float maxHeight = 0.0f; ///< 変位の全範囲を含むローカル Y の上限。
    float horizontalDisplacement = 0.0f; ///< 水平変位を覆うローカル XZ 方向の距離上限。
};

struct SurfacePatchView {
    math::Vector3 cameraPosition = math::Vector3::ZERO; ///< カメラのワールド位置 [m]。
    math::Vector3 cameraForward = { 0.0f, 0.0f, 1.0f }; ///< 視線方向の距離を測るワールド空間のカメラ前方向。
    float projectionScalePixels = 540.0f; ///< 透視では viewportHeight / (2*tan(verticalFov/2)) [px]、平行投影では画素密度 [px/m]。
    uint32_t viewportHeight = 0; ///< 選択キャッシュで照合する描画領域の高さ [px]。
    float minProjectionDistance = 0.1f; ///< 透視の投影セル幅を求める際に使う深度の下限 [m]。
    bool orthographic = false; ///< 画面上のセル幅を平行投影で計算するかどうか。
    const math::Frustum* frustum = nullptr; ///< 選択中だけ借用するワールド視錐台。nullptr ならカリングを省く。
    bool includeCulledPatches = false; ///< 視錐台外の葉も境界接続や外周を組むために出力するかどうか。
};

struct SurfacePatch {
    uint32_t depth = 0; ///< 基準矩形を 0 とした、このパッチの四分木深度。
    uint32_t x = 0; ///< 深度 depth の格子内での X 方向パッチ番号。
    uint32_t z = 0; ///< 深度 depth の格子内での Z 方向パッチ番号。
    math::Vector2 localMin; ///< 基準面でパッチが占めるローカル XZ の最小点。
    math::Vector2 localMax; ///< 基準面でパッチが占めるローカル XZ の最大点。
    math::Vector2 uvMin; ///< 基準矩形全体を [0, 1] としたパッチ UV の最小点。
    math::Vector2 uvMax; ///< 基準矩形全体を [0, 1] としたパッチ UV の最大点。
    math::Vector3 aabbMin; ///< 水平・垂直変位を含むローカル境界の最小点。
    math::Vector3 aabbMax; ///< 水平・垂直変位を含むローカル境界の最大点。
    math::Vector2 cellSize; ///< 波・高さの帯域制限に使う各軸のセル幅 [m]。
    math::Vector2 morphCellSize; ///< LOD 遷移で使うパッチ固有の幾何セル幅 [m]。隣接辺の共有帯域とは独立。
    uint8_t coarseEdgeMask = 0; ///< 粗いパッチと接する辺を示す SurfacePatchEdge のビット集合。
    std::array<math::Vector2, 4> edgeCellSize{}; ///< MIN_X / MAX_X / MIN_Z / MAX_Z の順。共有辺に接する最も粗いセル幅 [m]。
    /// @note MIN_X_MIN_Z / MAX_X_MIN_Z / MIN_X_MAX_Z / MAX_X_MAX_Z の順。
    /// @note 視錐台外も含め、同じ点に接する全パッチの最大セル幅を持つ。
    std::array<math::Vector2, 4> cornerCellSize{}; ///< 同じ角に接するパッチの最大セル幅 [m]。
};

struct SurfacePatchSelection {
    std::vector<SurfacePatch> patches; ///< 当該ビューと分割設定で選んだ葉パッチ。
    bool qualityLimited = false; ///< 最大深度またはパッチ予算によって要求密度まで分割できなかったかどうか。
};

/// @brief 同じ入力値と浮動小数点制御状態のパッチ選択を再利用する有界 cache。
/// @note Pure geometry only; camera/frustum are copied into the key and no Scene or GPU resource owner is retained.
class SurfacePatchSelectionCache {
public:
    /// @brief 入力値が一致する選択を返す。
    /// @return cache hit なら選択への非所有ポインター。cache を変更すると無効になり得る。
    [[nodiscard]] FBZZ_GRAPHICS_API const SurfacePatchSelection* Find(const SurfacePatchSurface& surface,
        const SurfacePatchLodSettings& settings, const SurfacePatchView& view);

    /// @brief Pins immutable geometry across nested callbacks, cache eviction and Clear.
    [[nodiscard]] FBZZ_GRAPHICS_API std::shared_ptr<const SurfacePatchSelection> FindOwned(const SurfacePatchSurface& surface,
        const SurfacePatchLodSettings& settings, const SurfacePatchView& view);

    /// @brief 正常に選択した結果を入力値と対応付けて保存する。
    /// @note 保存枠は最大 8 件・patch array capacity の合計 16384。超過時は最古の選択を追い出す。
    FBZZ_GRAPHICS_API void Store(const SurfacePatchSurface& surface, const SurfacePatchLodSettings& settings,
        const SurfacePatchView& view, const SurfacePatchSelection& selection);

    /// @brief Owns a completed selection without copying its patch array.
    /// @return An invocation owner even when invalid input or the retention budget prevents caching.
    [[nodiscard]] FBZZ_GRAPHICS_API std::shared_ptr<const SurfacePatchSelection> Store(const SurfacePatchSurface& surface,
        const SurfacePatchLodSettings& settings, const SurfacePatchView& view, SurfacePatchSelection&& selection);

    /// @brief 保持している選択を破棄する。
    FBZZ_GRAPHICS_API void Clear();

    /// @brief Transfers exclusive invocation storage without borrowing pointers into another cache.
    FBZZ_GRAPHICS_API void Swap(SurfacePatchSelectionCache& other) noexcept;

private:
    struct Entry {
        std::array<uint32_t, 80> signature{}; ///< 形状、ビュー、分割設定、浮動小数点制御状態を比較するキーの記憶領域。
        uint32_t signatureSize = 0; ///< signature 内の有効な uint32 要素数。
        std::shared_ptr<const SurfacePatchSelection> selection; ///< キャッシュ退役後も呼び出し中に保持できる不変の選択結果。
        uint64_t lastUse = 0; ///< 退役候補の順番を決める、この項目の最終参照番号。
    };

    std::vector<Entry> m_entries; ///< このキャッシュが保持するキーと選択結果の組。
    std::size_t m_cachedPatchCapacity = 0; ///< 保持したパッチ配列の予約要素数の合計。
    uint64_t m_useClock = 0; ///< 利用順を付ける単調増加の内部番号。
};

/// @brief Keeps pure geometry for at most two independent render views between invocations.
/// @note One recording thread owns this object; a busy view or a fully borrowed owner uses the caller's local cache.
class SurfacePatchSelectionCacheOwner {
public:
    SurfacePatchSelectionCacheOwner() = default;
    SurfacePatchSelectionCacheOwner(const SurfacePatchSelectionCacheOwner&) = delete;
    SurfacePatchSelectionCacheOwner& operator=(const SurfacePatchSelectionCacheOwner&) = delete;
    struct ViewKey {
        uint64_t resourceOwner = 0; ///< ビュー資源の所有元を照合する ResourceManager の識別番号。
        uint64_t resourceEpoch = 0; ///< 資源のリセット前後を区別する世代。
        uint64_t sceneGeneration = 0; ///< シーン切り替え前後を区別する世代。
        uint32_t view = 0; ///< 同じ資源管理元で独立したビューを区別する番号。
        uint32_t outputId = 0; ///< ビューの出力ターゲットのスロット添字。
        uint32_t outputGeneration = 0; ///< ビューの出力ターゲットのスロット世代。
        bool operator==(const ViewKey&) const = default;
    };
private:
    struct Slot {
        ViewKey key; ///< 保持中のパッチ選択を作ったビューの識別情報。
        SurfacePatchSelectionCache cache; ///< 呼び出し間で保持するビューのパッチ選択。
        uint64_t lastUse = 0; ///< ビューが最後に借りたときの内部番号。
        bool occupied = false; ///< スロットがビューに割り当てられているかどうか。
        bool inUse = false; ///< 実行中の呼び出しが対応するキャッシュを借りているかどうか。
        bool discardOnReturn = false; ///< Clear 後に、借り出し中のキャッシュを返却時に破棄するかどうか。
    };
public:
    class Lease {
    public:
        FBZZ_GRAPHICS_API ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    private:
        friend class SurfacePatchSelectionCacheOwner;
        FBZZ_GRAPHICS_API Lease(SurfacePatchSelectionCacheOwner& owner, const ViewKey& key,
            SurfacePatchSelectionCache& execution);
        Slot* m_slot = nullptr; ///< 借りている所有元スロット。ローカルキャッシュ使用時は null。
        SurfacePatchSelectionCache* m_execution = nullptr; ///< キャッシュを交換した呼び出し側の実行用領域。
    };

    /// @brief Borrows one view's cache and restores the caller's prior cache at invocation exit.
    /// @pre This owner and execution outlive the returned lease.
    [[nodiscard]] FBZZ_GRAPHICS_API Lease Borrow(const ViewKey& key, SurfacePatchSelectionCache& execution);
    /// @brief Clears idle views and discards active view storage when its lease returns.
    FBZZ_GRAPHICS_API void Clear();
private:
    std::array<Slot, 2> m_slots; ///< 呼び出し間で保持する、独立した 2 ビューのキャッシュ枠。
    uint64_t m_useClock = 0; ///< ビュー枠の再利用候補を比べるための単調増加の番号。
};

/// @brief 有限矩形を距離と投影セル幅で分割し、辺の隣接深度差を 1 以下へ保つ。
/// @param[in] surface 分割する基準矩形、変位の範囲、ワールド変換。
/// @param[in] settings 共通格子の解像度、分割予算と分割条件。
/// @param[in] view カメラ、投影条件と視錐台。
/// @param[out] out 選択した葉パッチと分割予算による品質制限の有無。
/// @return 無効な入力または float に収まらない導出幾何なら false。out は未変更。
/// @note cellsPerPatch は 2..256 の二の冪、maxDepth は 0..10、maxPatches は 1..16384。
/// @note world 行列は affine、各軸の長さの二乗は正の有限値。用途固有の scale 下限は持たない。
/// @note 予算には視錐台外のパッチも含める。分割不可なら粗い面を保持し、穴を作らない。
/// @note 巨大な投影要求は倍精度で評価する。幾何が有限なら失敗ではなく予算による qualityLimited とする。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 shared grid footprints and 2.3.3 patch bounds.
[[nodiscard]] FBZZ_GRAPHICS_API bool SelectSurfacePatches(const SurfacePatchSurface& surface,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view,
    SurfacePatchSelection& out);

/// @brief パッチの変位込み境界がビュー視錐台と交差するか調べる。
/// @param[in] surface パッチ選択に使った変位境界と world 行列。
/// @param[in] patch 成功した選択から得たパッチ。
/// @param[in] view 視錐台。nullptr なら可視として扱う。
/// @return 視錐台と交差する、または視錐台なしなら true。
/// @pre patch は surface と同じ入力の成功した選択から得る。
[[nodiscard]] FBZZ_GRAPHICS_API bool IsSurfacePatchVisible(const SurfacePatchSurface& surface,
    const SurfacePatch& patch, const SurfacePatchView& view);

/// @brief 正規化した (cellsPerPatch+1)^2 格子に、粗い隣接辺を縫合するインデックスを生成する。
/// @return 無効な分割数または mask なら false。outIndices は未変更。
/// @note 頂点順は Z 行優先、各頂点は X/Z = (x,z)/cellsPerPatch。表面の法線は +Y。
/// @note mask の辺では奇数の辺頂点を省き、隣接の二倍幅の格子と一致させる。内部は 2x2 セルの中心から張る。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 watertight regular-grid boundaries.
[[nodiscard]] FBZZ_GRAPHICS_API bool BuildSurfacePatchIndices(uint32_t cellsPerPatch, uint8_t coarseEdgeMask,
    std::vector<uint32_t>& outIndices);

/// @brief 縫合済みパッチの面と巻き順を保ち、リストと同じ面順のストリップを生成する。
/// @return 無効な分割数または mask なら false。outIndices は未変更。
/// @note 0xFFFFFFFF は 32 ビットの strip cut。
[[nodiscard]] FBZZ_GRAPHICS_API bool BuildSurfacePatchStripIndices(uint32_t cellsPerPatch, uint8_t coarseEdgeMask,
    std::vector<uint32_t>& outIndices);

/// @brief 固定格子の各セルを (左上, 左下, 右上) と (右上, 左下, 右下) に分けるストリップ。
/// @return 各軸のセル数が 1..512 の外なら false。outIndices は未変更。
/// @note 行間は 0xFFFFFFFF で区切る。頂点は Z 行優先。
[[nodiscard]] FBZZ_GRAPHICS_API bool BuildRegularSurfaceStripIndices(uint32_t cellsX, uint32_t cellsZ,
    std::vector<uint32_t>& outIndices);

/// @brief 同じ境界頂点を共有するパッチが使う波・高さ評価のセル幅を返す。
/// @pre unitUv は共有格子の正規化座標 [0,1]。
/// @note 内部は cellSize、辺は edgeCellSize、角は cornerCellSize。辺上で角の影響を補間しない。
/// @note 境界から内部への高さの遷移は三角形の補間へ任せ、異なる辺の長さによる帯域の不一致を防ぐ。
/// @warning GPU の同名評価も同じ辺・角の順序にすること。
[[nodiscard]] FBZZ_GRAPHICS_API math::Vector2 ResolveSurfacePatchCellSize(const SurfacePatch& patch,
    math::Vector2 unitUv);

} // namespace fbzz::renderer
