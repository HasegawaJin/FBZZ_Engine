/// @file    RenderDebugOverlay.cpp
/// @brief   主要 RT サムネイル (HDR/LDR/GBuffer) とパスタイミングを表示するデバッグオーバーレイ。
/// @author  Hasegawa Jin
/// @date    2026-05-28
///
/// 呼び出しタイミングの分離について:
/// GetImTextureID は DX11 の SRV バインド状態を変化させる可能性があるため、
/// GPU レンダリング中 (RenderSystem 内) から直接呼ぶと DrawIndexed でクラッシュする。
/// そのため RenderSystem では UpdateSnapshot() でハンドルだけ保存し、
/// GPU レンダリング完了後の ImGui フレーム内で DrawIfEnabled() を呼ぶ 2 ステップ構成にしている。
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>

#include <Engine/Renderer/ResourceManager.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace fbzz::renderer {

/// 静的スナップショット
/// UpdateSnapshot() で書き込み、DrawIfEnabled() で読み出す。
/// RenderSystem が GPU 実行中に書き、ImGui フレーム内で読む設計なので
/// シングルスレッド前提 (エンジン全体のスレッドモデルに準拠)。

namespace {

struct StoredState {
    RenderDebugOverlay::Snapshot snap;
    bool enabled = false;
};
static StoredState s_state;

} // namespace (closed after Draw impl)

void RenderDebugOverlay::UpdateSnapshot(const Snapshot& snapshot, bool enabled)
{
    s_state.snap    = snapshot;
    s_state.enabled = enabled;
}

const RenderDebugOverlay::Snapshot& RenderDebugOverlay::GetLastSnapshot()
{
    return s_state.snap;
}

void RenderDebugOverlay::DrawIfEnabled(IImGuiRenderer& imguiRenderer, ResourceManager& resources)
{
    if (!s_state.enabled) return;
    Draw(imguiRenderer, resources, s_state.snap);
}

namespace {

/// サムネイルの幅 (px)。高さはスクリーンのアスペクト比から算出する。
constexpr float THUMB_W = 200.0f;

/// タイミングバーチャートの最大幅 (px)。最長パスがこの幅になるよう正規化する。
constexpr float BAR_MAX_W = 180.0f;

/// バーの高さ (px)。
constexpr float BAR_H = 12.0f;

/// ImTextureID へ変換するヘルパー。GetImTextureID が返す void* を ImGui が要求する型に合わせる。
/// reinterpret_cast を uintptr_t 経由で行うことで、ポインタ幅の差異を吸収する。
ImTextureID ToImTexID(void* ptr)
{
    /// @note ImTextureID はこのプロジェクトでは整数型として定義されているため static_cast を使う。
    ///       ViewportPanel.cpp と同様のパターン。
    return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(ptr));
}

} // namespace

void RenderDebugOverlay::Draw(IImGuiRenderer& imguiRenderer, ResourceManager& resources,
                              const Snapshot& snapshot)
{
    /// @note アスペクト比を元に高さを決める。解像度が未設定なら 16:9 をフォールバックとする。
    const float thumbH = (snapshot.width > 0 && snapshot.height > 0)
        ? THUMB_W * (static_cast<float>(snapshot.height) / static_cast<float>(snapshot.width))
        : THUMB_W * 9.0f / 16.0f;

    /// @note GetImTextureID は DX11 の SRV バインド状態を変化させる可能性があるため、
    ///       ループを 2 回回すと SRV が交互に切り替わり表示がちらつく。
    ///       1 回だけ呼んで ImTextureID をキャッシュしてから ImGui に渡す。
    struct ResolvedSlot {
        const char* label;
        ImTextureID texID = 0;
    };
    ResolvedSlot resolved[3];
    int validSlotCount = 0;

    auto tryResolve = [&](const char* label, const ResourceHandle<RenderTargetTag>& rt) {
        if (!rt.IsValid()) return;
        void* rawID = imguiRenderer.GetImTextureID(rt, resources, 0);
        if (!rawID) return;
        resolved[validSlotCount++] = { label, ToImTexID(rawID) };
    };
    tryResolve("HDR",     snapshot.hdrRT);
    tryResolve("LDR",     snapshot.ldrRT);
    tryResolve("GBuffer", snapshot.gbufferRT);

    const float timingH = snapshot.passTimings.empty()
        ? 0.0f
        : static_cast<float>(snapshot.passTimings.size()) * (BAR_H + 4.0f) + 30.0f;
    const float initW = validSlotCount > 0
        ? THUMB_W * static_cast<float>(validSlotCount) + 16.0f * static_cast<float>(validSlotCount - 1) + 20.0f
        : 300.0f;
    const float initH = thumbH + 36.0f + timingH;

    ImGui::SetNextWindowSize({ initW, initH }, ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.90f);
    ImGui::Begin("Render Debug##passvwr", nullptr, ImGuiWindowFlags_NoScrollbar);

    /// @name 上段: RT サムネイルタイル
    if (validSlotCount > 0) {
        for (int i = 0; i < validSlotCount; ++i) {
            if (i > 0) ImGui::SameLine(0.0f, 16.0f);
            const auto& slot = resolved[i];

            ImGui::BeginGroup();
            ImGui::TextUnformatted(slot.label);
            ImGui::Image(slot.texID, { THUMB_W, thumbH });

            /// @note ホバー時に拡大プレビューをツールチップとして表示する。
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Image(slot.texID, { THUMB_W * 2.5f, thumbH * 2.5f });
                ImGui::EndTooltip();
            }

            ImGui::EndGroup();
        }
    } else {
        ImGui::TextDisabled("No render targets available.");
    }

    /// @name 下段: パス CPU / GPU タイミングバーチャート
    if (!snapshot.passTimings.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Pass timings  [CPU | GPU]");
        ImGui::Spacing();

        /// @note GPU 時間を名前引きできるよう map に変換する。
        std::unordered_map<std::string, double> gpuMap;
        for (const auto& [name, ms] : snapshot.gpuPassTimings)
            gpuMap[name] = ms;

        /// @note 最長パスを 1.0 として正規化する。
        double maxMs = 0.0;
        for (const auto& [name, ms] : snapshot.passTimings)
            maxMs = std::max(maxMs, ms);
        if (maxMs <= 0.0) maxMs = 1.0;

        /// @note 同名パスが複数あれば警告色で強調する (Editor が Scene/Game 両方から同一パスを呼ぶ等)。
        std::unordered_map<std::string, int> nameCount;
        for (const auto& [name, ms] : snapshot.passTimings)
            ++nameCount[name];

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        for (const auto& [name, ms] : snapshot.passTimings) {
            const bool isDup = nameCount[name] > 1;

            const ImVec2 cursor = ImGui::GetCursorScreenPos();

            /// @note CPU バー (重複=赤、通常=青)
            const float cpuW = static_cast<float>(ms / maxMs) * BAR_MAX_W;
            drawList->AddRectFilled(cursor,
                                    { cursor.x + cpuW, cursor.y + BAR_H },
                                    isDup ? IM_COL32(255, 80, 80, 200)
                                          : IM_COL32(80, 160, 255, 200));
            ImGui::Dummy({ BAR_MAX_W + 8.0f, BAR_H });
            ImGui::SameLine();

            auto gpuIt = gpuMap.find(name);
            if (isDup) {
                const ImVec4 red{ 1.0f, 0.35f, 0.35f, 1.0f };
                if (gpuIt != gpuMap.end()) {
                    ImGui::TextColored(red, "%-20s  CPU %.3f ms  GPU %.3f ms  [DUP]",
                                       name.c_str(), ms, gpuIt->second);
                } else {
                    ImGui::TextColored(red, "%-20s  CPU %.3f ms  [DUP]",
                                       name.c_str(), ms);
                }
            } else {
                if (gpuIt != gpuMap.end()) {
                    ImGui::Text("%-20s  CPU %.3f ms  GPU %.3f ms",
                                name.c_str(), ms, gpuIt->second);
                } else {
                    ImGui::Text("%-20s  CPU %.3f ms", name.c_str(), ms);
                }
            }
        }
    }

    ImGui::End();
}

} // namespace fbzz::renderer
