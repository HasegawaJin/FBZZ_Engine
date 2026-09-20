/// @file    RenderDebugOverlay.hpp
/// @brief   パスごとの RT サムネイルと CPU タイミングを表示する ImGui デバッグオーバーレイ。
/// @author  Hasegawa Jin
/// @date    2026-05-28
///
/// RenderSettings::passViewerEnabled が true のとき RenderSystem の末尾から呼ばれる。
/// ImGui フレーム内 (ImGui::NewFrame() と ImGui::Render() の間) での呼び出しが前提。
#pragma once

#include "ResourceHandle.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::renderer {

class IImGuiRenderer;
class ResourceManager;

/// パスごとの RT を ImGui ウィンドウにサムネイルタイルで表示するデバッグユーティリティ。
/// DebugDraw と同様にスタティックユーティリティとして使い、所有権は持たない。
class RenderDebugOverlay {
public:
    /// 表示するレンダーターゲットとタイミングのスナップショット。
    /// RenderSystem が各パス終了後にハンドルを詰めて Draw() に渡す。
    /// 無効なハンドルは自動でスキップされるため、パイプライン種別 (Forward/Deferred)
    /// に関わらず同じ構造体を使いまわせる。
    /// 1 フレームあたりのレンダリング統計。RenderSystem が集計し Snapshot に詰める。
    struct RenderStats {
        /// @note カリング前の描画候補オブジェクト数
        int totalObjects    = 0;
        /// @note フラスタムカリングで除外した数
        int frustumCulled   = 0;
        /// @note オクルージョンカリングで除外した数
        int occlusionCulled = 0;
        /// @note 描画距離 (Max Draw Distance / Layer Cull Distances) で除外した数
        int distanceCulled    = 0;
        /// @note 画面上で小さすぎるとして除外した数
        int smallObjectCulled = 0;
        /// @note カメラ視点で実際に発行した DrawCall 数 (不透明・半透明合計)
        int drawCalls       = 0;
        /// @note 描画した総頂点数
        int vertexCount     = 0;
        /// @note 描画した総三角形数 (indexCount / 3)
        int triangleCount   = 0;
        /// @note SkinningComputePass の処理頂点数
        uint64_t skinningVertexCount = 0;
        /// @note SkinningComputePass の Dispatch 数
        uint32_t skinningDispatchCount = 0;
        /// @note シャドウマップは同じジオメトリを光源視点で再描画する別パスのため、
        ///       カメラ統計に混ぜず内訳として分けて表示する。
        int shadowDrawCalls     = 0;
        int shadowTriangleCount = 0;
        /// @note 束ねて発行した Instanced Draw の回数と、それで減ったドロー数。
        /// @see Docs/design/gpu-instancing.md
        int instancedBatches    = 0;
        int instancedDrawsSaved = 0;
    };

    struct Snapshot {
        ResourceHandle<RenderTargetTag> hdrRT;           ///< HDR カラーバッファ (フォワード/ディファード共通)
        ResourceHandle<RenderTargetTag> ldrRT;           ///< トーンマップ後 LDR バッファ
        ResourceHandle<RenderTargetTag> selectionMaskRT; ///< 選択オブジェクトマスク
        ResourceHandle<RenderTargetTag> outlineRT;       ///< 選択アウトラインバッファ
        ResourceHandle<RenderTargetTag> gbufferRT;       ///< G-Buffer (ディファードのみ有効)

        /// RenderGraph::ExecutionReport::profiles から詰めた {パス名, CPU時間(ms)} リスト。
        /// 空のままにするとタイミング表示をスキップする。
        std::vector<std::pair<std::string, double>> passTimings;

        /// GpuProfGetResults() から詰めた {パス名, GPU時間(ms)} リスト。
        /// QUERY_LATENCY フレーム分溜まるまでは空。
        std::vector<std::pair<std::string, double>> gpuPassTimings;

        uint32_t width  = 0;
        uint32_t height = 0;

        /// レンダリング統計 (Stats UI で使用)
        RenderStats renderStats;

        /// RenderGraph::DescribeLastPlan() の結果。実行順・カリング・エイリアス割り当てを
        /// 1 行 1 項目で並べたテキストで、計測値は含まない。
        /// @note 実行順が変わったことは絵を見ても分からず «差分が取れること» が唯一の価値なので、
        ///       毎フレーム動く数値は混ぜない。パス構成が変わったフレームだけ更新されるため、
        ///       そのままでは前回の内容が残る。
        std::string planDescription;
    };

    /// 最後の UpdateSnapshot() で保存されたスナップショットを返す。
    /// ViewportPanel の Stats オーバーレイから renderStats を読む用途を想定。
    static const Snapshot& GetLastSnapshot();

    /// RenderSystem 末尾から呼ぶ。ハンドルを静的領域に保存するだけで ImGui / GPU に触れない。
    /// ImGui フレーム外 (GPU レンダリング中) から安全に呼べる。
    static void UpdateSnapshot(const Snapshot& snapshot, bool enabled);

    /// ImGui フレーム内・GPU レンダリング完了後に呼ぶ (EditorApp::RenderPanels 等)。
    /// UpdateSnapshot で enabled=false が渡されていれば即リターンする。
    static void DrawIfEnabled(IImGuiRenderer& imguiRenderer, ResourceManager& resources);

private:
    /// 実際の ImGui 描画ロジック。DrawIfEnabled からのみ呼ぶ。
    static void Draw(IImGuiRenderer& imguiRenderer, ResourceManager& resources,
                     const Snapshot& snapshot);
};

} // namespace fbzz::renderer
