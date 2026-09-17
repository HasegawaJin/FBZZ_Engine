/// @file    TerrainTool.hpp
/// @brief   エディタ上でマウス操作により地形の高さ彫刻（Sculpt）とテクスチャ塗布（Paint）を行うツール。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note レイキャスト・ブラシ計算・フォールオフを TerrainTool 単一クラスへ集約する。ブラシ操作→heightData/splatData 変更→dirty フラグ→GPU 再転送のフローを Script/Panel に分散させない。
/// @note 呼び出し順: ViewportPanel::OnRenderContent 末尾で Update()、OnEditorGUI() でブラシ設定 UI。heightDirty/splatDirty は自動で立つ。
/// @note 依存: Engine (TerrainComponent, Ray, Camera, Scene), ImGui。
#pragma once

#include "TerrainBrush.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>
#include <Math/Ray.hpp>
#include <functional>
#include <string>
#include <vector>
#include <imgui.h>

namespace fbzz::renderer { class IRenderer; }

namespace fbzz::editor {

class UndoStack;

class TerrainTool {
public:
    /// @name ブラシモード
    /// @{
    enum class Mode {
        /// @note 高さ彫刻
        Sculpt,
        /// @note スプラットマップ塗布
        Paint,
    };
    /// @}

    /// @name Sculpt サブモード / フォールオフ / ブラシ形状
    /// @{
    /// @note 実体は TerrainBrush.hpp のカーネル型の別名。AI (Command Bus) も同じブラシを叩くため型と適用ロジックの正本をカーネル側へ移した。
    /// @note 別型にして相互変換を書くと enum 追加時に片方が黙って既定値へ落ちる。別名なので TerrainTool::SculptMode::Raise の書き方と保存済み int 値は変わらない。
    using SculptMode    = TerrainSculptOp;
    using FalloffType   = TerrainFalloff;
    using BrushSettings = TerrainBrush;
    /// @}

    /// @name 公開 API
    /// @{

    /// @brief 毎フレームシーンビューで呼ぶ。マウス入力を処理して地形を編集する。
    /// @param scene TerrainComponent を含むシーン。
    /// @param camera エディタカメラ (レイキャスト用)。
    /// @param dt フレーム時間 (ブラシ強度に乗算)。
    /// @param viewportHovered マウスがビューポート上にあるか。
    /// @param viewportMin ビューポートの左上スクリーン座標。
    /// @param viewportSize ビューポートのサイズ。
    /// @param markDirty シーンの dirty フラグを立てるコールバック。
    void Update(scene::Scene&             scene,
                const renderer::Camera&   camera,
                float                     dt,
                bool                      viewportHovered,
                const ImVec2&             viewportMin,
                const ImVec2&             viewportSize,
                const std::function<void()>& markDirty,
                UndoStack*                 undoStack);

    /// ImGui でブラシ設定 UI を描画する。
    /// ViewportPanel の内側（ImGui::Begin スコープ内）から呼ぶこと。
    void OnEditorGUI(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);

    /// NatureTool 向け API — ウィンドウを持たないタブコンテンツ描画
    void DrawSculptContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawPaintContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawImportSection(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawBrushSettings();

    /// NatureTool 向けアクセサ
    void          SetActive(bool a)       { m_active = a; }
    bool          IsActive()        const { return m_active; }
    void          SetMode(Mode m)         { m_mode = m; }
    Mode          GetMode()         const { return m_mode; }
    BrushSettings GetBrush()        const { return m_brush; }
    void          SetBrush(float radius, float strength, FalloffType falloff) {
        m_brush.radius = radius; m_brush.strength = strength; m_brush.falloff = falloff;
    }
    SculptMode    GetSculptMode()   const { return m_sculpt; }
    void          SetSculptMode(SculptMode s) { m_sculpt = s; }
    uint32_t      GetPaintLayer()   const { return m_paintLayer; }
    void          SetPaintLayer(uint32_t layer) { m_paintLayer = layer; }

    /// @brief マウス位置からレイを飛ばして地形と交差判定する。
    /// @note ヒットしたら outHitWorld (ワールド座標) と outGO を設定して true を返す。Scene View のクリックピッキング (PickEntity) からも地形選択に使うため public。
    bool RaycastTerrain(scene::Scene&           scene,
                        const renderer::Camera& camera,
                        const ImVec2&           viewportMin,
                        const ImVec2&           viewportSize,
                        math::Vector3&          outHitWorld,
                        scene::GameObject*&     outGO) const;
    /// @}

private:
    /// @name 状態
    /// @{
    bool        m_active  = false;  ///< false のとき入力処理・ブラシ描画をスキップする
    Mode        m_mode    = Mode::Sculpt;
    SculptMode  m_sculpt  = SculptMode::Raise;
    /// @note 修飾キーで一時的に切り替わる実効サブモード。Update() が毎フレーム設定し、ブラシ適用時はこちらを渡す。Shift+drag=Smooth / Ctrl+drag=Lower をパネルへ視線を戻さず使え、修飾を離せば m_sculpt に戻る。
    SculptMode  m_activeSculpt = SculptMode::Raise;
    BrushSettings m_brush;

    uint32_t m_paintLayer    = 0;      ///< Paint モード: 塗るレイヤーインデックス [0, 3]
    float    m_flattenTarget = 0.0f;   ///< Flatten モード: 基準高さ（ワールド単位）
    bool     m_flattenLocked = false;  ///< Flatten モード: 最初のクリックで固定したか
    struct TerrainStrokeSnapshot {
        std::string instanceId;
        scene::TerrainComponent before;
    };
    scene::EntityID         m_strokeEntity;
    std::string             m_strokeInstanceId;
    scene::TerrainComponent m_strokeBefore;
    std::vector<TerrainStrokeSnapshot> m_strokeBeforeTerrains;
    bool                    m_strokeActive = false;

    bool              m_isHovering = false;  ///< 地形にマウスが乗っているか
    math::Vector3     m_hitPoint;            ///< レイキャスト結果（ワールド座標）
    scene::GameObject* m_hitTerrain = nullptr; ///< ヒットした地形 GO（非所有）
    /// @}

    /// @name HeightMap Import
    /// @{
    char        m_heightMapPath[512] = {};       ///< インポートするファイルパス（InputText 用）
    bool        m_heightMapUnipolar  = true;     ///< true: [0,maxH]  false: [-maxH,+maxH]
    std::string m_heightMapStatus;               ///< "OK" / "Error: ..." / "" (未実行)
    /// @}

    /// @name 内部ヘルパー
    /// @{

    /// DDA + 二分探法でレイと地形メッシュの交差を求める内部処理。
    /// localHit はテレインローカル座標で返す。
    bool RaycastSingleTerrain(const math::Ray&                 ray,
                               const scene::TerrainComponent&   terrain,
                               const scene::Transform&          tf,
                               math::Vector3&                   outLocalHit) const;

    /// Sculpt / Paint の適用とフォールオフ計算は TerrainBrush.hpp のカーネル
    /// (ApplyTerrainSculpt / ApplyTerrainPaint / TerrainBrushWeight) を直接呼ぶ。

    /// ブラシ円をビューポートに ImGui DrawList で描画する（スクリーン空間投影）
    void DrawBrushPreview(const ImVec2&           viewportMin,
                          const ImVec2&           viewportSize,
                          const renderer::Camera& camera) const;
    /// @}
};

} // namespace fbzz::editor
