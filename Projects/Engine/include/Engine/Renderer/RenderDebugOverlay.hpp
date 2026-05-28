// FBZZ Engine
// RenderDebugOverlay.hpp | fbzz::renderer
// パスごとの RT サムネイルと CPU タイミングを表示する ImGui デバッグオーバーレイ。
// RenderSettings::passViewerEnabled が true のとき RenderSystem の末尾から呼ばれる。
// ImGui フレーム内 (ImGui::NewFrame() と ImGui::Render() の間) での呼び出しが前提。
#pragma once

#include "ResourceHandle.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::renderer {

class IRenderer;
class ResourceManager;

// パスごとの RT を ImGui ウィンドウにサムネイルタイルで表示するデバッグユーティリティ。
// DebugDraw と同様にスタティックユーティリティとして使い、所有権は持たない。
class RenderDebugOverlay {
public:
    // 表示するレンダーターゲットとタイミングのスナップショット。
    // RenderSystem が各パス終了後にハンドルを詰めて Draw() に渡す。
    // 無効なハンドルは自動でスキップされるため、パイプライン種別 (Forward/Deferred)
    // に関わらず同じ構造体を使いまわせる。
    struct Snapshot {
        ResourceHandle<RenderTargetTag> hdrRT;           // HDR カラーバッファ (フォワード/ディファード共通)
        ResourceHandle<RenderTargetTag> ldrRT;           // トーンマップ後 LDR バッファ
        ResourceHandle<RenderTargetTag> selectionMaskRT; // 選択オブジェクトマスク
        ResourceHandle<RenderTargetTag> outlineRT;       // 選択アウトラインバッファ
        ResourceHandle<RenderTargetTag> gbufferRT;       // G-Buffer (ディファードのみ有効)

        // RenderGraph::ExecutionReport::profiles から詰めた {パス名, CPU時間(ms)} リスト。
        // 空のままにするとタイミング表示をスキップする。
        std::vector<std::pair<std::string, double>> passTimings;

        uint32_t width  = 0;
        uint32_t height = 0;
    };

    // RenderSystem 末尾から呼ぶ。ハンドルを静的領域に保存するだけで ImGui / DX11 に触れない。
    // ImGui フレーム外 (GPU レンダリング中) から安全に呼べる。
    static void UpdateSnapshot(const Snapshot& snapshot, bool enabled);

    // ImGui フレーム内・GPU レンダリング完了後に呼ぶ (EditorApp::RenderPanels 等)。
    // UpdateSnapshot で enabled=false が渡されていれば即リターンする。
    static void DrawIfEnabled(IRenderer& renderer, ResourceManager& resources);

private:
    // 実際の ImGui 描画ロジック。DrawIfEnabled からのみ呼ぶ。
    static void Draw(IRenderer& renderer, ResourceManager& resources,
                     const Snapshot& snapshot);
};

} // namespace fbzz::renderer
