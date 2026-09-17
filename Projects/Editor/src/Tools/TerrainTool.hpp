/// @file    TerrainTool.hpp
/// @brief   エディタ上でマウス操作により地形の高さ彫刻・層の塗布・坂・穴を編集するツール。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note レイキャスト・ストローク・Undo を TerrainTool に集約し、ブラシの式は TerrainBrush.hpp のカーネルへ委ねる (AI バスと同じ実体)。
/// @note 呼び出し順: ViewportPanel::OnRenderContent 末尾で Update()、パネル側で Draw*Content()。dirty フラグは Update() が立てる。
/// @see Docs/design/terrain-layers.md
#pragma once

#include "TerrainBrush.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>
#include <Math/Ray.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <imgui.h>

namespace fbzz::renderer { class IRenderer; }

namespace fbzz::editor {

class UndoStack;

/// @brief Scene View 上の地形編集ツール。
class TerrainTool {
public:
    /// @brief ブラシモード。保存済み int 値を変えないため追加は末尾へ置く。
    enum class Mode {
        Sculpt, ///< 高さ彫刻 (Ramp もここに含む)
        Paint,  ///< スプラット塗布
        Hole,   ///< 穴を開ける / 埋める
    };

    /// @name カーネル型の別名
    /// @note 別型にして相互変換を書くと enum 追加時に片方が黙って既定値へ落ちる。別名なので保存済み int 値は変わらない。
    /// @{
    using SculptMode    = TerrainSculptOp;
    using FalloffType   = TerrainFalloff;
    using BrushSettings = TerrainBrush;
    /// @}

    /// @return Sculpt サブモードの表示名。範囲外は "?"。
    static const char* SculptModeLabel(SculptMode mode);

    /// @brief 毎フレームシーンビューで呼ぶ。マウス入力を処理して地形を編集する。
    /// @param dt フレーム時間 [s]。ブラシ強度に乗算する (Ramp と Hole には掛けない)。
    /// @param viewportMin ビューポートの左上スクリーン座標。
    /// @param markDirty シーンの dirty フラグを立てるコールバック。
    /// @param undoStack null ならストロークを Undo へ積まない。
    void Update(scene::Scene&             scene,
                const renderer::Camera&   camera,
                float                     dt,
                bool                      viewportHovered,
                const ImVec2&             viewportMin,
                const ImVec2&             viewportSize,
                const std::function<void()>& markDirty,
                UndoStack*                 undoStack);

    /// @brief スタンドアローンのツールウィンドウを描く。
    /// @pre ViewportPanel の ImGui::Begin スコープ内から呼ぶこと。
    void OnEditorGUI(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);

    /// @name ウィンドウを持たないタブ内容 (MapEditorPanel から呼ぶ)
    /// @{
    void DrawSculptContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawPaintContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawHoleContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    /// @note 取り込みと書き出しの対象はシーン内の最初の有効な地形。
    void DrawImportSection(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawBrushSettings();
    /// @}

    /// @name アクセサ
    /// @{
    void          SetActive(bool a)       { m_active = a; }
    bool          IsActive()        const { return m_active; }
    void          SetMode(Mode m)         { m_mode = m; }
    Mode          GetMode()         const { return m_mode; }
    BrushSettings GetBrush()        const { return m_brush; }
    void          SetBrush(float radius, float strength, FalloffType falloff) {
        m_brush.radius = radius; m_brush.strength = strength; m_brush.falloff = falloff;
    }
    SculptMode    GetSculptMode()   const { return m_sculpt; }
    /// @note サブモードを選ぶと Ramp は解除される。
    void          SetSculptMode(SculptMode s) { m_sculpt = s; m_rampMode = false; }
    bool          IsRampMode()      const { return m_rampMode; }
    void          SetRampMode(bool ramp)  { m_rampMode = ramp; }
    bool          IsHoleErase()     const { return m_holeErase; }
    void          SetHoleErase(bool erase) { m_holeErase = erase; }
    uint32_t      GetPaintLayer()   const { return m_paintLayer; }
    void          SetPaintLayer(uint32_t layer) { m_paintLayer = layer; }
    /// @return Sculpt ならサブモード名 (Ramp 含む)、Paint なら "Layer N"、Hole なら "Cut" / "Fill"。
    std::string   StatusLabel() const;
    /// @}

    /// @brief マウス位置からレイを飛ばして地形と交差判定する。
    /// @note Scene View のクリックピッキングからも地形選択に使うため public。穴のセルでは交差しない。
    /// @return ヒットしたら outHitWorld (ワールド座標) と outGO を設定して true。
    bool RaycastTerrain(scene::Scene&           scene,
                        const renderer::Camera& camera,
                        const ImVec2&           viewportMin,
                        const ImVec2&           viewportSize,
                        math::Vector3&          outHitWorld,
                        scene::GameObject*&     outGO) const;

private:
    /// @name 状態
    /// @{
    bool        m_active  = false;  ///< false のとき入力処理・ブラシ描画をスキップする
    Mode        m_mode    = Mode::Sculpt;
    SculptMode  m_sculpt  = SculptMode::Raise;
    /// @note 修飾キーで一時的に切り替わる実効サブモード。Shift+drag=Smooth / Ctrl+drag=Lower をパネルへ視線を戻さず使える。
    SculptMode  m_activeSculpt = SculptMode::Raise;
    BrushSettings m_brush;

    bool     m_rampMode      = false;  ///< Sculpt の中で Ramp を選んでいるか (SculptOp ではないので別に持つ)
    bool     m_holeErase     = false;  ///< Hole: true なら穴を埋める
    uint32_t m_paintLayer    = 0;      ///< Paint: 塗る層番号。参照地形の層数 - 1 へ丸める
    float    m_flattenTarget = 0.0f;   ///< Flatten: 基準高さ (ローカル Y [m])
    bool     m_flattenLocked = false;  ///< Flatten: 最初のクリックで固定したか

    struct TerrainStrokeSnapshot {
        std::string instanceId;
        scene::TerrainComponent before;
    };
    std::vector<TerrainStrokeSnapshot> m_strokeBeforeTerrains;
    bool          m_strokeActive = false;
    std::uint32_t m_strokeStep   = 0;   ///< ストローク内の適用回数。侵食の乱数を回ごとに変える

    bool          m_rampDragging = false;
    math::Vector3 m_rampStartWorld;
    math::Vector3 m_rampEndWorld;

    bool              m_isHovering = false;    ///< 地形にマウスが乗っているか
    math::Vector3     m_hitPoint;              ///< レイキャスト結果 (ワールド座標)
    scene::GameObject* m_hitTerrain = nullptr; ///< ヒットした地形 GO (非所有)
    /// @}

    /// @name HeightMap 入出力
    /// @{
    char        m_heightMapPath[512]   = {};    ///< インポートするファイルパス (InputText 用)
    char        m_heightMapExport[512] = {};    ///< 書き出し先 (InputText 用)
    int         m_heightMapUnipolar    = 1;     ///< 1: [0,maxH]  0: [-maxH,+maxH] (RadioButton が int を要求する)
    std::string m_heightMapStatus;              ///< "OK" / "Error: ..." / "" (未実行)
    std::string m_heightMapExportStatus;        ///< 書き出し結果
    /// @}

    /// @brief DDA + 二分探索でレイと地形の交差を求める。穴のセルの交差は飛ばして次を探す。
    /// @param outLocalHit テレインローカル座標。
    bool RaycastSingleTerrain(const math::Ray&                 ray,
                               const scene::TerrainComponent&   terrain,
                               const scene::Transform&          tf,
                               math::Vector3&                   outLocalHit) const;

    /// @brief ストロークで初めて触れる地形の変更前を取っておく。
    void CaptureStrokeBefore(scene::GameObject& go, const scene::TerrainComponent& terrain);
    /// @brief ストロークを確定し、触れた地形を 1 つの Undo にまとめる。
    /// @param rebuildCollider 形が変わる操作 (Sculpt / Ramp / Hole) なら true。
    void CommitStroke(scene::Scene& scene, const char* undoLabel, bool rebuildCollider,
                      const std::function<void()>& markDirty, UndoStack* undoStack);
    /// @brief Ramp の押下・ドラッグ・確定。
    void UpdateRamp(scene::Scene& scene, bool mousePressed, bool mouseReleased,
                    const std::function<void()>& markDirty, UndoStack* undoStack);

    /// @brief ブラシ円をビューポートに ImGui DrawList で描く (スクリーン空間投影)。
    void DrawBrushPreview(const ImVec2&           viewportMin,
                          const ImVec2&           viewportSize,
                          const renderer::Camera& camera) const;
    /// @brief Ramp の線分と幅をビューポートに描く。
    void DrawRampPreview(const ImVec2&           viewportMin,
                         const ImVec2&           viewportSize,
                         const renderer::Camera& camera) const;
};

} // namespace fbzz::editor
