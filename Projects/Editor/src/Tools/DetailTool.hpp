// FBZZ Engine
// DetailTool.hpp | fbzz::editor
// Terrain Detail System のインタラクティブペイントツール。
//
// 機能:
//   Paint/Erase モードでビューポート上のブラシ操作により、
//   各 DetailLayer の密度マップ (DetailDensityMap) を編集する。
//   ブラシを離したタイミングで影響チャンクを再 Bake する。
//
// 使い方:
//   1. ViewportPanel が毎フレーム Update() を呼ぶ
//   2. ctx.showDetailTool が true のとき OnEditorGUI() を呼ぶ
//   3. EditorApp が唯一のインスタンスを所有し、EditorContext 経由でポインタを公開する
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>
#include <Math/Ray.hpp>
#include <functional>
#include <string>
#include <vector>
#include <imgui.h>

namespace fbzz::editor {

class UndoStack;

class DetailTool {
public:
    // ビューポートで毎フレーム呼ぶ。入力処理 + ブラシ描画 + チャンクデバッグ表示。
    // undoStack を渡すとブラシストロークを 1 操作として Undo 履歴へ積む (nullptr で無効)。
    void Update(scene::Scene&             scene,
                const renderer::Camera&   camera,
                bool                      viewportHovered,
                const ImVec2&             viewportMin,
                const ImVec2&             viewportSize,
                const std::function<void()>& markDirty,
                UndoStack*                undoStack = nullptr);

    // ImGui ツールウィンドウを描画する。
    void OnEditorGUI(scene::Scene& scene, const std::function<void()>& markDirty);

    // NatureTool 向け: ウィンドウなしでタブコンテンツだけ描画する
    void DrawContent(scene::Scene& scene, const std::function<void()>& markDirty);

    void SetActive(bool a) { m_active = a; }
    bool IsActive()  const { return m_active; }
    void SetBrush(float radius, float strength) {
        m_brush.radius = radius; m_brush.strength = strength;
    }
    float GetBrushRadius()     const { return m_brush.radius; }
    float GetBrushStrength()   const { return m_brush.strength; }
    int   GetMode()            const { return static_cast<int>(m_mode); }
    void  SetMode(int m)             { m_mode = static_cast<Mode>(m); }
    int   GetLayerIndex()      const { return m_layerIndex; }
    void  SetLayerIndex(int i)       { m_layerIndex = i; }
    bool  GetShowChunkBounds() const { return m_showChunkBounds; }
    void  SetShowChunkBounds(bool v) { m_showChunkBounds = v; }
    bool  GetShowCounts()      const { return m_showCounts; }
    void  SetShowCounts(bool v)      { m_showCounts = v; }

private:
    enum class Mode { Paint, Erase };

    struct BrushSettings {
        float radius   = 6.0f;  // ブラシ半径 [ワールド単位]
        float strength = 0.6f;  // 1 フレームあたりの最大変化量 [0, 1]
    };

    bool          m_active     = false;
    Mode          m_mode       = Mode::Paint;
    int           m_layerIndex = 0;
    BrushSettings m_brush;
    bool          m_showChunkBounds = false;
    bool          m_showCounts      = false;

    // ホバー状態
    bool             m_isHovering = false;
    math::Vector3    m_hitPoint   = {};
    scene::EntityID  m_hitEntity  = {};
    bool             m_strokeDirty = false;
    scene::EntityID  m_strokeEntity = {};

    // Undo 用ストロークスナップショット。ストローク開始時に密度マップ全体を退避し、
    // 終了時に before/after を LambdaCommand へ渡す。instanceId(guid) で対象を再解決する。
    bool                                   m_strokeCaptured = false;
    std::string                            m_strokeInstanceId;
    std::vector<scene::DetailDensityMap>   m_strokeBeforeMaps;

    // レイキャスト (TerrainTool と同じアルゴリズム)
    bool RaycastTerrain(scene::Scene&           scene,
                        const renderer::Camera& camera,
                        const ImVec2&           viewportMin,
                        const ImVec2&           viewportSize,
                        math::Vector3&          outHitWorld,
                        scene::EntityID&        outEntity) const;

    bool RaycastSingleTerrain(const math::Ray&               ray,
                              const scene::TerrainComponent&  terrain,
                              const scene::Transform&         tf,
                              math::Vector3&                  outLocalHit) const;

    // 密度マップが未初期化なら地形サイズに合わせて作成する。
    // WHY: Paint は空のマップから追加し、Erase は均一配置から削るため、
    //      操作モードに応じて初期密度を切り替える。
    void EnsureDensityMap(scene::TerrainDetailComponent& detail,
                          const scene::TerrainComponent& terrain,
                          int layerIdx) const;

    // ブラシを密度マップに適用し、値が変化した場合だけ true を返す。
    [[nodiscard]] bool ApplyBrush(scene::TerrainDetailComponent& detail,
                                  const scene::TerrainComponent& terrain,
                                  const scene::Transform&        tf,
                                  const math::Vector3&           hitWorld,
                                  float                          dt);

    // ブラシ円をビューポートに描画する
    void DrawBrushPreview(const ImVec2&           viewportMin,
                          const ImVec2&           viewportSize,
                          const renderer::Camera& camera) const;

    // チャンク境界とインスタンス数をビューポートにオーバーレイ描画する
    void DrawChunkDebug(scene::Scene&           scene,
                        const ImVec2&           viewportMin,
                        const ImVec2&           viewportSize,
                        const renderer::Camera& camera) const;
};

} // namespace fbzz::editor
