/// @file    TerrainTool.hpp
/// @brief   エディタ上でマウス操作により地形の高さ彫刻（Sculpt）とテクスチャ塗布（Paint）を行うツール。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHY: 地形編集は「ブラシ操作 → heightData / splatData 変更 → dirty フラグ → GPU 再転送」
/// という明確なデータフローを持つ。これを Script や Panel に分散させず、
/// TerrainTool という単一クラスに集約することで、レイキャスト・ブラシ計算・
/// フォールオフ・アルゴリズムを一箇所で管理できる。
///
/// 使い方:
/// 1. ViewportPanel::OnRenderContent のシーンビュー処理末尾で Update() を呼ぶ
/// 2. TerrainTool::OnEditorGUI() でブラシ設定 UI を表示する
/// 3. heightDirty / splatDirty は TerrainTool が自動的に立てる
///
/// 依存: Engine (TerrainComponent, Ray, Camera, Scene), ImGui
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
    // ── ブラシモード ──────────────────────────────────────────────────────────
    enum class Mode {
        Sculpt, // 高さ彫刻
        Paint,  // スプラットマップ塗布
    };

    // ── Sculpt サブモード / フォールオフ / ブラシ形状 ─────────────────────────
    // 実体は TerrainBrush.hpp のカーネル型。ここでは従来の名前を別名として保つ。
    // WHY: 同じブラシを AI (Command Bus の terrain.sculpt / terrain.paint) も叩くため、
    //      型と適用ロジックの正本をカーネル側へ移した。別型にして相互変換を書くと、
    //      片方に enum を足したときにもう片方が黙って既定値へ落ちる。別名なので
    //      TerrainTool::SculptMode::Raise という既存の書き方と保存済み int 値は変わらない。
    using SculptMode    = TerrainSculptOp;
    using FalloffType   = TerrainFalloff;
    using BrushSettings = TerrainBrush;

    // ── 公開 API ──────────────────────────────────────────────────────────────

    // 毎フレームシーンビューで呼ぶ。マウス入力を処理して地形を編集する。
    //
    // 引数:
    //   scene          - TerrainComponent を含むシーン
    //   camera         - エディタカメラ（レイキャスト用）
    //   dt             - フレーム時間（ブラシ強度に乗算する）
    //   viewportHovered - マウスがビューポート上にあるか
    //   viewportMin    - ビューポートの左上スクリーン座標
    //   viewportSize   - ビューポートのサイズ
    //   markDirty      - シーンの dirty フラグを立てるコールバック
    void Update(scene::Scene&             scene,
                const renderer::Camera&   camera,
                float                     dt,
                bool                      viewportHovered,
                const ImVec2&             viewportMin,
                const ImVec2&             viewportSize,
                const std::function<void()>& markDirty,
                UndoStack*                 undoStack);

    // ImGui でブラシ設定 UI を描画する。
    // ViewportPanel の内側（ImGui::Begin スコープ内）から呼ぶこと。
    void OnEditorGUI(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);

    // NatureTool 向け API — ウィンドウを持たないタブコンテンツ描画
    void DrawSculptContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawPaintContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawImportSection(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawBrushSettings();

    // NatureTool 向けアクセサ
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

    // マウス位置からレイを飛ばして地形と交差判定する。
    // ヒットしたら outHitWorld（ワールド座標）と outGO を設定して true を返す。
    // WHY: Scene View のクリックピッキング (PickEntity) からも地形選択に使うため public。
    bool RaycastTerrain(scene::Scene&           scene,
                        const renderer::Camera& camera,
                        const ImVec2&           viewportMin,
                        const ImVec2&           viewportSize,
                        math::Vector3&          outHitWorld,
                        scene::GameObject*&     outGO) const;

private:
    // ── 状態 ──────────────────────────────────────────────────────────────────
    bool        m_active  = false;  // false のとき入力処理・ブラシ描画をスキップする
    Mode        m_mode    = Mode::Sculpt;
    SculptMode  m_sculpt  = SculptMode::Raise;
    // 修飾キーで一時的に切り替わる実効サブモード。Update() が毎フレーム設定し、
    // ブラシ適用時はこちらを渡す。WHY: Unity の Terrain と同じく Shift+drag=Smooth /
    //      Ctrl+drag=Lower をパネルへ視線を戻さず使えるようにする。修飾を離せば m_sculpt に戻る。
    SculptMode  m_activeSculpt = SculptMode::Raise;
    BrushSettings m_brush;

    uint32_t m_paintLayer    = 0;      // Paint モード: 塗るレイヤーインデックス [0, 3]
    float    m_flattenTarget = 0.0f;   // Flatten モード: 基準高さ（ワールド単位）
    bool     m_flattenLocked = false;  // Flatten モード: 最初のクリックで固定したか
    struct TerrainStrokeSnapshot {
        std::string instanceId;
        scene::TerrainComponent before;
    };
    scene::EntityID         m_strokeEntity;
    std::string             m_strokeInstanceId;
    scene::TerrainComponent m_strokeBefore;
    std::vector<TerrainStrokeSnapshot> m_strokeBeforeTerrains;
    bool                    m_strokeActive = false;

    bool              m_isHovering = false;  // 地形にマウスが乗っているか
    math::Vector3     m_hitPoint;            // レイキャスト結果（ワールド座標）
    scene::GameObject* m_hitTerrain = nullptr; // ヒットした地形 GO（非所有）

    // ── HeightMap Import ──────────────────────────────────────────────────────
    char        m_heightMapPath[512] = {};       // インポートするファイルパス（InputText 用）
    bool        m_heightMapUnipolar  = true;     // true: [0,maxH]  false: [-maxH,+maxH]
    std::string m_heightMapStatus;               // "OK" / "Error: ..." / "" (未実行)

    // ── 内部ヘルパー ──────────────────────────────────────────────────────────

    // DDA + 二分探法でレイと地形メッシュの交差を求める内部処理。
    // localHit はテレインローカル座標で返す。
    bool RaycastSingleTerrain(const math::Ray&                 ray,
                               const scene::TerrainComponent&   terrain,
                               const scene::Transform&          tf,
                               math::Vector3&                   outLocalHit) const;

    // Sculpt / Paint の適用とフォールオフ計算は TerrainBrush.hpp のカーネル
    // (ApplyTerrainSculpt / ApplyTerrainPaint / TerrainBrushWeight) を直接呼ぶ。

    // ブラシ円をビューポートに ImGui DrawList で描画する（スクリーン空間投影）
    void DrawBrushPreview(const ImVec2&           viewportMin,
                          const ImVec2&           viewportSize,
                          const renderer::Camera& camera) const;
};

} // namespace fbzz::editor
