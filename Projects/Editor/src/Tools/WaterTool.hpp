/// @file    WaterTool.hpp
/// @brief   エディタ上で WaterComponent の水面パラメータを視覚的に編集するツール。
/// @author  Hasegawa Jin
/// @date    2026-06-01
///
/// WHY: TerrainTool がブラシ操作に特化しているのと同様に、WaterTool は水面固有の
/// 編集操作（波プリセット適用・ビューポート上の範囲/波向き可視化）を一箇所に集約する。
/// Inspector の WaterComponent セクションはパラメータ列挙にとどめ、
/// WaterTool はより直感的な視覚フィードバックを提供する。
///
/// 使い方:
/// 1. ViewportPanel::OnRenderContent のシーンビュー処理末尾で Update() を呼ぶ
/// 2. WaterTool::OnEditorGUI() でツールウィンドウを表示する
/// 3. water.meshDirty / foamDirty / texDirty は WaterTool が自動的に立てる
///
/// 依存: Engine (WaterComponent, Camera, Scene), ImGui
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>
#include <functional>
#include <string>
#include <imgui.h>

namespace fbzz::editor {

class UndoStack;

class WaterTool {
public:
    // ── 公開 API ──────────────────────────────────────────────────────────────

    // 毎フレームシーンビューで呼ぶ。水面の範囲・波向きをビューポートに可視化する。
    //
    // 引数:
    //   scene           - WaterComponent を含むシーン
    //   camera          - エディタカメラ（スクリーン投影用）
    //   viewportMin     - ビューポートの左上スクリーン座標
    //   viewportSize    - ビューポートのサイズ
    //   markDirty       - シーンの dirty フラグを立てるコールバック
    void Update(scene::Scene&                scene,
                const renderer::Camera&      camera,
                const ImVec2&                viewportMin,
                const ImVec2&                viewportSize,
                const std::function<void()>& markDirty);

    // ImGui でツールウィンドウを描画する。
    // ViewportPanel の内側（ImGui::Begin スコープ内）から呼ぶこと。
    void OnEditorGUI(scene::Scene& scene, const std::string& projectRoot,
                     const std::function<void()>& markDirty,
                     UndoStack* undoStack);

    // NatureTool 向け: ウィンドウなしでタブコンテンツだけ描画する
    void DrawContent(scene::Scene& scene, const std::string& projectRoot,
                     const std::function<void()>& markDirty, UndoStack* undoStack);

    void SetActive(bool a) { m_active = a; }
    bool IsActive()  const { return m_active; }

private:
    // ── 状態 ──────────────────────────────────────────────────────────────────
    bool      m_active = false;          // false のとき可視化・UI をスキップ
    uint32_t  m_selectedWaterIndex = 0; // 複数水面があるとき選択中のインデックス

    // ── 内部ヘルパー ──────────────────────────────────────────────────────────

    // ワールド座標をスクリーン座標へ投影するラムダを返す（各フレーム生成）
    // WHY: TerrainTool の DrawBrushPreview と同じパターン。
    //      3D → 2D 投影を ImGui DrawList で描画するために使う。
    static ImVec2 ProjectToScreen(
        const math::Vector3&    p,
        const math::Matrix4&    vp,
        const ImVec2&           vpMin,
        const ImVec2&           vpSize);

    // 水面の AABB 矩形をビューポートにワイヤーフレーム描画する
    void DrawWaterBounds(
        const scene::WaterComponent& water,
        const scene::Transform&      transform,
        const math::Matrix4&         vp,
        const ImVec2&                vpMin,
        const ImVec2&                vpSize) const;

    // 有効な Gerstner 波の方向を矢印でビューポートに描画する
    void DrawWaveArrows(
        const scene::WaterComponent& water,
        const scene::Transform&      transform,
        const math::Matrix4&         vp,
        const ImVec2&                vpMin,
        const ImVec2&                vpSize) const;

    // ツールウィンドウのアセット管理セクション
    void DrawAssetSection(scene::WaterComponent& water,
                          const std::string&     scenePath,
                          const std::string&     projectRoot,
                          const std::function<void()>& markDirty);

    // ツールウィンドウの波プリセットセクション
    void DrawPresets(scene::WaterComponent& water,
                     const std::function<void()>& markDirty) const;

    // .mat の視覚パラメータを Water 向けプリセットとして調整する
    void DrawLookPresets(scene::WaterComponent& water,
                         const std::string& projectRoot,
                         const std::function<void()>& markDirty) const;

    // .mat の視覚パラメータをその場でスライダー編集する (色 / 透明度 / さざ波 / 泡)。
    // WHY: 以前はプリセットボタンしかなく、詰めるには .mat を Inspector で開き直す必要があった。
    //      水面は見ながら追い込むものなので、ビューポート横で直接触れるようにする。
    void DrawSurfaceEditor(scene::WaterComponent& water,
                           const std::string& projectRoot,
                           const std::function<void()>& markDirty) const;

    // ツールウィンドウの波エディタセクション
    void DrawWaveEditor(scene::WaterComponent& water,
                        const std::function<void()>& markDirty) const;

    // DrawContent / OnEditorGUI の共通本体（対象選択 + 波エディタ）
    void DrawContentBody(scene::Scene& scene, const std::string& projectRoot,
                         const std::function<void()>& markDirty, UndoStack* undoStack);
};

} // namespace fbzz::editor
