// FBZZ Engine
// TerrainTool.hpp | fbzz::editor
// エディタ上でマウス操作により地形の高さ彫刻（Sculpt）とテクスチャ塗布（Paint）を行うツール
//
// WHY: 地形編集は「ブラシ操作 → heightData / splatData 変更 → dirty フラグ → GPU 再転送」
//      という明確なデータフローを持つ。これを Script や Panel に分散させず、
//      TerrainTool という単一クラスに集約することで、レイキャスト・ブラシ計算・
//      フォールオフ・アルゴリズムを一箇所で管理できる。
//
// 使い方:
//   1. ViewportPanel::OnRenderContent のシーンビュー処理末尾で Update() を呼ぶ
//   2. TerrainTool::OnEditorGUI() でブラシ設定 UI を表示する
//   3. heightDirty / splatDirty は TerrainTool が自動的に立てる
//
// 依存: Engine (TerrainComponent, Ray, Camera, Scene), ImGui
#pragma once

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

    // ── Sculpt サブモード ─────────────────────────────────────────────────────
    enum class SculptMode {
        Raise,   // 高さを上げる
        Lower,   // 高さを下げる
        Smooth,  // 周囲と平滑化
        Flatten, // 最初にクリックした高さに揃える
        Stamp,   // ブラシ形状を押し付ける（height = max(h, weight)）
    };

    // ── ブラシのフォールオフ形状 ──────────────────────────────────────────────
    enum class FalloffType {
        Linear,   // 距離に比例して線形減衰
        Smooth,   // smoothstep（端が滑らか）
        Gaussian, // ガウス曲線（自然な盛り上がり）
    };

    struct BrushSettings {
        float       radius   = 5.0f;             // ブラシ半径（ワールド単位）
        float       strength = 0.05f;            // 1 フレームあたりの最大変化量 [0, 1]
        FalloffType falloff  = FalloffType::Smooth;
    };

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
    // ApplySculpt はこちらを参照する。WHY: Unity の Terrain と同じく Shift+drag=Smooth /
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

    // ブラシを TerrainComponent の高さデータに適用する（Sculpt モード）
    void ApplySculpt(scene::TerrainComponent& terrain,
                     const math::Vector3&     hitLocal,
                     float                    dt) const;

    // SampleAvg4: Smooth モードで使う上下左右 4 近傍平均
    float SampleAvg4(const scene::TerrainComponent& t, int x, int z) const;

    // ブラシをスプラットマップに適用する（Paint モード）
    void ApplyPaint(scene::TerrainComponent& terrain,
                    const math::Vector3&     hitLocal,
                    float                    dt,
                    int                      layerIndex) const;

    // ブラシ中心からの距離 dist に対してフォールオフウェイト [0, 1] を返す
    float ComputeWeight(float dist) const;

    // ブラシ円をビューポートに ImGui DrawList で描画する（スクリーン空間投影）
    void DrawBrushPreview(const ImVec2&           viewportMin,
                          const ImVec2&           viewportSize,
                          const renderer::Camera& camera) const;
};

} // namespace fbzz::editor
