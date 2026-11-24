/// @file    RenderSceneMaterialDemand.hpp
/// @brief   ビューのマテリアル需要と遅延バインディングの補完。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include "MaterialDemandTrace.hpp"
#include <optional>

namespace fbzz::scene::material_demand_detail {

/// @note 同一スレッドの抽出深度を復元する。再入中は需要を限定できない。
class ExtractionDepthScope {
public:
    ExtractionDepthScope();
    ~ExtractionDepthScope();
    ExtractionDepthScope(const ExtractionDepthScope&) = delete;
    ExtractionDepthScope& operator=(const ExtractionDepthScope&) = delete;
};

/// @note 現在の描画スコープが公開スナップショットを共有所有し、利用境界で必要になれば省略分を補う。
void RetainPendingBindings(const RenderPassContext& context, std::shared_ptr<const renderer::RenderScene> snapshot,
    std::vector<uint32_t> omittedItemIndices);
/// @note 省略対象のインデックスが維持されるスナップショット置換に限る。需要なしの判定結果は失効する。
void RetargetBindings(const std::shared_ptr<const renderer::RenderScene>& previous,
    const std::shared_ptr<const renderer::RenderScene>& completed);

}

namespace fbzz::scene::render_extraction_detail {

/// @note 取得した行は同一スレッドで更新し、スコープ終了時に集計へ渡す。
class MaterialDemandTraceScope {
public:
    explicit MaterialDemandTraceScope(const RenderPassContext& context);
    MaterialDemandTraceScope(const MaterialDemandTraceScope&) = delete;
    MaterialDemandTraceScope& operator=(const MaterialDemandTraceScope&) = delete;
    ~MaterialDemandTraceScope();
    [[nodiscard]] bool Active() const { return m_row.has_value(); }
    /// @pre Active() が true。参照はこのスコープが生存する間だけ有効。
    [[nodiscard]] material_demand_detail::Row& Counters() { return *m_row; }
    void Complete(const RenderPassContext& context);
    /// @pre Active() が true で、対象の syncEligibleItems を加算済み。
    void CountPotential(const material_demand_detail::View& view, const renderer::RenderMeshItem& item,
        const material_demand_detail::Result& result);
private:
    std::optional<material_demand_detail::Row> m_row;
    uint32_t m_index = 0;
    bool m_tracked = false;
};

[[nodiscard]] bool CurrentRayMaterialInputs(const RenderPassContext& context);
/// @note 判定結果の視錐台は context から借用する。資源コールバックを越えて利用条件を保持しない。
/// @see Docs/design/runtime-workset-optimization.md
[[nodiscard]] material_demand_detail::View CurrentMaterialDemandView(const RenderPassContext& context,
    bool rayTracingInputs, bool completing = false);
void RepairViewMaterialBindings(RenderPassContext& context, renderer::RenderScene& snapshot,
    std::vector<uint32_t>& omittedItemIndices, uint64_t resourceFrame, uint64_t resourceResetVersion,
    MaterialDemandTraceScope& trace);

}
