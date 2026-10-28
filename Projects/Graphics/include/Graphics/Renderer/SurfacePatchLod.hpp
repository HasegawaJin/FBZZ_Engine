/// @file    SurfacePatchLod.hpp
/// @brief   有限の連続面を共有格子で描くためのビュー別パッチ LOD と境界インデックス。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once

#include <Math/Matrix4.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
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
    uint32_t cellsPerPatch = 16;
    uint32_t maxDepth = 8;
    uint32_t maxPatches = 4096;
    /// @note morphEnabled はパッチ境界と同じ高さ場を保つ画面密度ベースのモーフを有効にする。
    bool morphEnabled = false;
    /// @note 親レベルの投影要求が 1 を越えてから詳細へ戻るまでの正規化幅。
    float morphTransitionWidth = 0.25f;
    /// @note 投影されたセルの幅 [pixel]。高さ関数の幾何誤差の上限ではない。
    float targetCellPixels = 24.0f;
    float nearDistance = 64.0f;
    float nearCellSize = 2.0f;
};

struct SurfacePatchSurface {
    /// @note localMin / localMax は基準矩形のローカル XZ。波による水平変位は含めない。
    math::Vector2 localMin = { -50.0f, -50.0f };
    math::Vector2 localMax = { 50.0f, 50.0f };
    math::Matrix4 worldMatrix = math::Matrix4::Identity();
    /// @note minHeight / maxHeight と horizontalDisplacement は全変位を包むローカル単位の上限。
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    float horizontalDisplacement = 0.0f;
};

struct SurfacePatchView {
    math::Vector3 cameraPosition = math::Vector3::ZERO;
    math::Vector3 cameraForward = { 0.0f, 0.0f, 1.0f };
    /// @note 透視では viewportHeight / (2*tan(verticalFov/2))、平行投影では pixel / world m。
    float projectionScalePixels = 540.0f;
    /// @note 選択 cache のビュー識別に使う。分割計算自体は投影スケールを使う。
    uint32_t viewportHeight = 0;
    float minProjectionDistance = 0.1f;
    bool orthographic = false;
    /// @note この呼び出し中だけ参照する。nullptr はカリングなし。
    const math::Frustum* frustum = nullptr;
    /// @note 可視判定を分割には使ったまま、skirt や外周生成用に不可視 leaf も出力する。
    bool includeCulledPatches = false;
};

struct SurfacePatch {
    /// @note depth / x / z は基準矩形を四分木で分けた安定キー。ビュー間で共有しない。
    uint32_t depth = 0;
    uint32_t x = 0;
    uint32_t z = 0;
    math::Vector2 localMin, localMax;
    math::Vector2 uvMin, uvMax;
    /// @note aabb は水平・垂直変位を含むローカル境界。localMin / localMax は基準面の区画。
    math::Vector3 aabbMin, aabbMax;
    /// @note セル幅は各軸のワールド m。境界の帯域制限に使う。
    math::Vector2 cellSize;
    /// @note パッチ固有の幾何セル幅。境界の波帯域や far strip の共有帯域とは独立する。
    math::Vector2 morphCellSize;
    uint8_t coarseEdgeMask = 0;
    /// @note MIN_X / MAX_X / MIN_Z / MAX_Z の順。辺を共有する最も粗いパッチのセル幅。
    std::array<math::Vector2, 4> edgeCellSize{};
    /// @note MIN_X_MIN_Z / MAX_X_MIN_Z / MIN_X_MAX_Z / MAX_X_MAX_Z の順。
    /// @note 視錐台外も含め、同じ点に接する全パッチの最大セル幅を持つ。
    std::array<math::Vector2, 4> cornerCellSize{};
};

struct SurfacePatchSelection {
    std::vector<SurfacePatch> patches;
    /// @note true は要求密度が maxDepth または maxPatches で制限されたことを示す。
    bool qualityLimited = false;
};

/// @brief 同じ入力値と浮動小数点制御状態のパッチ選択を再利用する有界 cache。
/// @note Pure geometry only; camera/frustum are copied into the key and no Scene or GPU resource owner is retained.
class SurfacePatchSelectionCache {
public:
    /// @brief 入力値が一致する選択を返す。
    /// @return cache hit なら選択への非所有ポインター。cache を変更すると無効になり得る。
    [[nodiscard]] const SurfacePatchSelection* Find(const SurfacePatchSurface& surface,
        const SurfacePatchLodSettings& settings, const SurfacePatchView& view);

    /// @brief Pins immutable geometry across nested callbacks, cache eviction and Clear.
    [[nodiscard]] std::shared_ptr<const SurfacePatchSelection> FindOwned(const SurfacePatchSurface& surface,
        const SurfacePatchLodSettings& settings, const SurfacePatchView& view);

    /// @brief 正常に選択した結果を入力値と対応付けて保存する。
    /// @note 保存枠は最大 8 件・patch array capacity の合計 16384。超過時は最古の選択を追い出す。
    void Store(const SurfacePatchSurface& surface, const SurfacePatchLodSettings& settings,
        const SurfacePatchView& view, const SurfacePatchSelection& selection);

    /// @brief Owns a completed selection without copying its patch array.
    /// @return An invocation owner even when invalid input or the retention budget prevents caching.
    [[nodiscard]] std::shared_ptr<const SurfacePatchSelection> Store(const SurfacePatchSurface& surface,
        const SurfacePatchLodSettings& settings, const SurfacePatchView& view, SurfacePatchSelection&& selection);

    /// @brief 保持している選択を破棄する。
    void Clear();

    /// @brief Transfers exclusive invocation storage without borrowing pointers into another cache.
    void Swap(SurfacePatchSelectionCache& other) noexcept;

private:
    struct Entry {
        std::array<uint32_t, 80> signature{};
        uint32_t signatureSize = 0;
        std::shared_ptr<const SurfacePatchSelection> selection;
        uint64_t lastUse = 0;
    };

    std::vector<Entry> m_entries;
    std::size_t m_cachedPatchCapacity = 0;
    uint64_t m_useClock = 0;
};

/// @brief Keeps pure geometry for at most two independent render views between invocations.
/// @note One recording thread owns this object; a busy view or a fully borrowed owner uses the caller's local cache.
class SurfacePatchSelectionCacheOwner {
public:
    SurfacePatchSelectionCacheOwner() = default;
    SurfacePatchSelectionCacheOwner(const SurfacePatchSelectionCacheOwner&) = delete;
    SurfacePatchSelectionCacheOwner& operator=(const SurfacePatchSelectionCacheOwner&) = delete;
    struct ViewKey {
        uint64_t resourceOwner = 0, resourceEpoch = 0, sceneGeneration = 0;
        uint32_t view = 0, outputId = 0, outputGeneration = 0;
        bool operator==(const ViewKey&) const = default;
    };
private:
    struct Slot {
        ViewKey key;
        SurfacePatchSelectionCache cache;
        uint64_t lastUse = 0;
        bool occupied = false, inUse = false, discardOnReturn = false;
    };
public:
    class Lease {
    public:
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    private:
        friend class SurfacePatchSelectionCacheOwner;
        Lease(SurfacePatchSelectionCacheOwner& owner, const ViewKey& key,
            SurfacePatchSelectionCache& execution);
        Slot* m_slot = nullptr;
        SurfacePatchSelectionCache* m_execution = nullptr;
    };

    /// @brief Borrows one view's cache and restores the caller's prior cache at invocation exit.
    /// @pre This owner and execution outlive the returned lease.
    [[nodiscard]] Lease Borrow(const ViewKey& key, SurfacePatchSelectionCache& execution);
    /// @brief Clears idle views and discards active view storage when its lease returns.
    void Clear();
private:
    std::array<Slot, 2> m_slots;
    uint64_t m_useClock = 0;
};

/// @brief 有限矩形を距離と投影セル幅で分割し、辺の隣接深度差を 1 以下へ保つ。
/// @return 無効な入力または float に収まらない導出幾何なら false。out は未変更。
/// @note cellsPerPatch は 2..256 の二の冪、maxDepth は 0..10、maxPatches は 1..16384。
/// @note world 行列は affine、各軸の長さの二乗は正の有限値。用途固有の scale 下限は持たない。
/// @note 予算には視錐台外のパッチも含める。分割不可なら粗い面を保持し、穴を作らない。
/// @note 巨大な投影要求は倍精度で評価する。幾何が有限なら失敗ではなく予算による qualityLimited とする。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 shared grid footprints and 2.3.3 patch bounds.
[[nodiscard]] bool SelectSurfacePatches(const SurfacePatchSurface& surface,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view,
    SurfacePatchSelection& out);

/// @brief パッチの変位込み境界がビュー視錐台と交差するか調べる。
/// @param surface パッチ選択に使った変位境界と world 行列。
/// @param patch 成功した選択から得たパッチ。
/// @param view 視錐台。nullptr なら可視として扱う。
/// @return 視錐台と交差する、または視錐台なしなら true。
/// @pre patch は surface と同じ入力の成功した選択から得る。
[[nodiscard]] bool IsSurfacePatchVisible(const SurfacePatchSurface& surface,
    const SurfacePatch& patch, const SurfacePatchView& view);

/// @brief 正規化した (cellsPerPatch+1)^2 格子に、粗い隣接辺を縫合するインデックスを生成する。
/// @return 無効な分割数または mask なら false。outIndices は未変更。
/// @note 頂点順は Z 行優先、各頂点は X/Z = (x,z)/cellsPerPatch。表面の法線は +Y。
/// @note mask の辺では奇数の辺頂点を省き、隣接の二倍幅の格子と一致させる。内部は 2x2 セルの中心から張る。
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 watertight regular-grid boundaries.
[[nodiscard]] bool BuildSurfacePatchIndices(uint32_t cellsPerPatch, uint8_t coarseEdgeMask,
    std::vector<uint32_t>& outIndices);

/// @brief 同じ境界頂点を共有するパッチが使う波・高さ評価のセル幅を返す。
/// @pre unitUv は共有格子の正規化座標 [0,1]。
/// @note 内部は cellSize、辺は edgeCellSize、角は cornerCellSize。辺上で角の影響を補間しない。
/// @note 境界から内部への高さの遷移は三角形の補間へ任せ、異なる辺の長さによる帯域の不一致を防ぐ。
/// @warning GPU の同名評価も同じ辺・角の順序にすること。
[[nodiscard]] math::Vector2 ResolveSurfacePatchCellSize(const SurfacePatch& patch,
    math::Vector2 unitUv);

}
