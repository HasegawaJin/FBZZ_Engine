/// @file    TerrainTool.hpp
/// @brief   エディタ上でマウス操作により地形の高さ彫刻・層の塗布・坂・穴を編集するツール。
/// @author  Hasegawa Jin
/// @date    2026-05-31
/// @note レイキャスト・ストローク・Undo を TerrainTool に集約し、ブラシの式は TerrainBrush.hpp のカーネルへ委ねる (AI バスと同じ実体)。
/// @note 呼び出し順: ViewportPanel::OnRenderContent 末尾で Update()、パネル側で Draw*Content()。dirty フラグは Update() が立てる。
/// @see Docs/design/terrain-layers.md
#pragma once
#include "TerrainScatter.hpp"

#include "TerrainBrush.hpp"
#include "TerrainAuthoring.hpp"
#include "TerrainPaintLibrary.hpp"
#include "TerrainPaintMix.hpp"
#include <Engine/Scene/Systems/RenderSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>
#include <Math/Ray.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <imgui.h>

namespace fbzz::renderer { class IRenderer; }

namespace fbzz::editor {

class UndoStack;
struct EditorContext;
class OpArgs;
class OpData;
struct TerrainHistorySnapshot;

/// @brief Scene View 上の地形編集ツール。
class TerrainTool {
public:
    /// @brief ブラシモード。保存済み int 値を変えないため追加は末尾へ置く。
    enum class Mode {
        Sculpt, ///< @note 高さ彫刻 (Ramp もここに含む)。
        Paint,
        Hole,
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
    /// @note Undo は context の現在シーンと部品の寿命を検証してから適用する。
    void Update(EditorContext&            context,
                const renderer::Camera&   camera,
                float                     dt,
                bool                      viewportHovered,
                const ImVec2&             viewportMin,
                const ImVec2&             viewportSize);

    /// @brief スタンドアローンのツールウィンドウを描く。
    /// @pre ViewportPanel の ImGui::Begin スコープ内から呼ぶこと。
    void OnEditorGUI(EditorContext& context);

    /// @name ウィンドウを持たないタブ内容 (MapEditorPanel から呼ぶ)
    /// @{
    void DrawSculptContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    void DrawPaintContent(EditorContext& context);
    void DrawPaintAssetInspector(EditorContext& context, const std::string& path);
    void OpenPaintAssetCreation(const std::string& directory);
    void SetPaintPalette(std::vector<std::string> sources, bool focusWindow);
    [[nodiscard]] std::span<const scene::RenderInstancePreview> PaintPreviewInputs(const scene::Scene& scene) const;
    [[nodiscard]] TerrainPaintMix CapturePaintMix(const std::string& name) const;
    void ApplyPaintMix(const TerrainPaintMix& mix);
    [[nodiscard]] bool SelectPaintInstance(EditorContext& context, const std::string& owner, uint32_t id);
    void DrawHoleContent(scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty);
    /// @note 取り込み・層生成・散布は共通の明示選択先へ適用する。初回に有効な地形が一個だけなら自動選択する。
    void DrawAuthoringSection(EditorContext& context);
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
    void          SetPaintLayer(uint32_t layer) { m_paintLayer = layer; m_paintTarget = PaintTarget::Materials; }
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
    [[nodiscard]] float BrushRadiusLimit() const;

    /// @name 状態
    /// @{
    bool        m_active  = false;
    Mode        m_mode    = Mode::Sculpt;
    SculptMode  m_sculpt  = SculptMode::Raise;
    /// @note 修飾キーで一時的に切り替わる実効サブモード。Shift+drag=Smooth / Ctrl+drag=Lower をパネルへ視線を戻さず使える。
    SculptMode  m_activeSculpt = SculptMode::Raise;
    BrushSettings m_brush;

    bool     m_rampMode      = false;  ///< @note SculptOp と別に保持する Sculpt のサブモード。
    bool     m_holeErase     = false;  ///< @note true なら穴を埋める。
    uint32_t m_paintLayer    = 0;      ///< @note 参照地形の層数 - 1 へ丸める。
    enum class PaintTarget { Materials, Grass, Instances, HeightMap };
    PaintTarget m_paintTarget = PaintTarget::Instances;
    TerrainPaintLibrary m_paintLibrary;
    char m_heightStampPath[512] = {};
    TerrainHeightStampSettings m_heightStamp;
    bool m_heightStampUnipolar = true;
    bool m_heightStampAnchor = true;
    std::string m_heightStampStatus;
    bool m_grassErase = false;
    int m_grassEraseLayer = -1;
    bool m_scatterErase = false;
    bool m_scatterStrokeErase = false;
    bool m_scatterStrokeContinuous = false;
    float m_scatterStrokeRadius = 0.0f;
    uint64_t m_scatterTerrainLifetime = 0;
    std::string m_scatterStrokeTerrainId;
    struct PaintSource {
        std::string reference;
        float weight = 100.0f;
    };
    std::vector<PaintSource> m_paintPalette;
    bool m_focusPaintPalette = false;
    bool m_showPaintPreview = true;
    double m_nextPaintPreviewTime = 0.0;
    size_t m_paintPreviewTotal = 0;
    std::vector<TerrainPaintPreviewPoint> m_paintPreview;
    std::vector<scene::RenderInstancePreview> m_paintPreviewInputs;
    uint64_t m_paintPreviewGeneration = 0;
    bool m_paintAvoidWater = true;
    bool m_paintAvoidPaths = true;
    int m_paintExcludedLayer = -1;
    float m_paintMinimumDistance = 0.5f;
    std::string m_paintMixName = "New Mix";
    std::string m_paintMixPath;
    std::vector<std::string> m_paintExcludedMaterials;
    float m_paintFeather = 0.2f;
    bool m_paintShowDiagnostics = true;
    TerrainPaintDiagnostics m_paintDiagnostics;
    int m_instanceMode = 0;
    std::vector<math::Vector3> m_paintArea;
    int m_editScope = 0;
    float m_editKeep = 0.7f, m_editScale = 1.0f, m_editHeight = 1.0f;
    std::string m_instanceOwner;
    uint32_t m_instanceId = 0;
    uint64_t m_instanceRevision = 0, m_instanceLifetime = 0;
    renderer::InstancePlacement m_instanceDraft;
    renderer::InstancePlacement m_instanceOriginal;
    bool m_instanceDragging = false;
    int m_instanceGizmo = 0;
    std::string m_paintPreviewError;
    std::string m_paintPreviewTerrainId;
    math::Vector3 m_paintPreviewCenter;
    std::vector<math::Vector3> m_scatterStroke;
    float    m_flattenTarget = 0.0f;   ///< @note 基準高さ (ローカル Y [m])。
    bool     m_flattenLocked = false;

    enum class TerrainStrokeChannel { HEIGHT, SPLAT, HOLES, COUNT };
    struct TerrainStrokeRow {
        int minX = 0, maxX = -1;
    };
    struct TerrainStrokeSnapshot {
        std::string instanceId;
        std::shared_ptr<const TerrainHistorySnapshot> before;
        uint64_t lifetime = 0;
        scene::EntityID entity;
        scene::TerrainHeightRegion edited;
        scene::TerrainHeightRegion heightRegion, splatRegion, holeRegion;
        bool fullPcgBounds = false;
        /// @note Per-channel row bounds keep diagonal dabs from scanning the entire stroke rectangle at commit.
        std::array<std::vector<TerrainStrokeRow>, static_cast<std::size_t>(TerrainStrokeChannel::COUNT)> rows;

        void Include(const scene::TerrainHeightRegion& region, TerrainStrokeChannel channel);
    };
    struct TerrainStrokeEdit;
    std::vector<TerrainStrokeSnapshot> m_strokeBeforeTerrains;
    std::unordered_map<std::string, std::size_t> m_strokeTerrainIndices;
    std::unordered_map<std::string, std::weak_ptr<const TerrainHistorySnapshot>> m_strokeBaselines;
    uint64_t m_strokeSceneGeneration = 0;
    bool          m_strokeActive = false;
    std::uint32_t m_strokeStep   = 0;   ///< @note 侵食の乱数をストローク内の適用回ごとに変える。

    bool          m_rampDragging = false;
    math::Vector3 m_rampStartWorld;
    math::Vector3 m_rampEndWorld;

    bool              m_isHovering = false;
    math::Vector3     m_hitPoint;              ///< @note ワールド座標。
    scene::GameObject* m_hitTerrain = nullptr; ///< @note ヒットした地形 GO の非所有参照。
    /// @}

    /// @name HeightMap 入出力
    /// @{
    char        m_heightMapPath[512]   = {};
    char        m_heightMapExport[512] = {};
    int         m_heightMapUnipolar    = 1;     ///< @note 1: [0,maxH]、0: [-maxH,+maxH]。
    std::string m_heightMapStatus;
    std::string m_heightMapExportStatus;
    std::string m_heightMapTargetId;
    std::uint64_t m_heightMapSceneGeneration = 0;
    bool m_heightMapResize = false;
    int m_heightMapColumns = 513;
    int m_heightMapRows = 513;
    float m_heightMapCellSize = 4.0f;
    float m_heightMapMaxHeight = 640.0f;
    int m_biomeGrassLayer = 0;
    int m_biomeRockLayer = 1;
    int m_biomeSnowLayer = 2;
    float m_biomeSnowHeight = 420.0f;
    float m_biomeSnowFade = 60.0f;
    float m_biomeRockSlope = 38.0f;
    float m_biomeSlopeFade = 10.0f;
    std::string m_biomeStatus;
    char m_grassMaterial[512] = "Assets/Terrain/Alpine/GrassCards.mat";
    float m_grassDensity = 4.0f;
    float m_grassBladeWidth = 0.5f;
    float m_grassDistance = 80.0f;
    std::string m_grassStatus;
    char m_layerMaskPath[512] = {};
    int m_layerMaskLayer = 0;
    float m_layerMaskStrength = 1.0f;
    bool m_layerMaskInvert = false;
    std::string m_layerMaskStatus;
    TerrainMapSettings m_mapSettings;
    int m_mapChannel = static_cast<int>(TerrainMapChannel::DENSITY);
    int m_mapLayer = 0;
    float m_mapStrength = 1.0f;
    bool m_mapIncludeNeighbors = true;
    bool m_flowCustomCenter = false;
    bool m_flowPreviewReady = false;
    bool m_flowBuildWater = true;
    bool m_flowBuildWaterfalls = false;
    float m_flowWaterfallDrop = DEFAULT_WATERFALL_DROP;
    float m_flowWaterfallGrade = DEFAULT_WATERFALL_GRADE;
    char m_flowWaterMaterial[512] = {};
    char m_terrainRecipePath[512] = "Assets/Terrain/Recipes/RiverValley.terrainrecipe.toml";
    std::string m_terrainRecipeStatus;
    std::string m_flowExpected;
    std::size_t m_flowOverlayUndoRevision = 0;
    std::array<std::vector<math::Vector3>, 3> m_flowPreviewPoints;
    std::vector<std::vector<math::Vector3>> m_flowPreviewWater;
    std::vector<std::array<math::Vector3, 2>> m_flowPreviewWaterfalls;
    std::vector<std::array<math::Vector3, 2>> m_flowPreviewWaterfallSections;
    TerrainHabitatSettings m_habitat;
    TerrainFlowCarveSettings m_flowCarve;
    std::vector<TerrainMaterialRule> m_naturalMaterials{TERRAIN_NATURAL_MATERIAL_RULES.begin(), TERRAIN_NATURAL_MATERIAL_RULES.end()};
    float m_naturalStrength = 1.0f;
    float m_naturalProtectedThreshold = 0.35f;
    std::vector<int> m_naturalProtectedLayers{-1};
    char m_naturalProtectionMask[512] = {};
    std::string m_flowCarveStatus;
    std::string m_naturalMaterialStatus;
    char m_mapExportPath[512] = {};
    std::string m_mapPreview;
    enum class MapOverlay { OFF, GENERATED, SAVED_LAYER, DENSITY_MASK };
    MapOverlay m_mapOverlay = MapOverlay::OFF;
    int m_mapOverlayLayer = 0;
    std::uint64_t m_mapPreviewLifetime = 0, m_mapPreviewSurface = 0, m_mapPreviewVegetation = 0, m_mapPreviewUndo = 0;
    math::Matrix4 m_mapPreviewWorld = math::Matrix4::Identity();
    TerrainMapRaster m_densityPreview;
    std::string m_densityPreviewPath;
    struct MapOverlaySample {
        math::Vector3 local;
        float value = 0.0f;
        bool valid = false;
    };
    struct MapOverlayCache {
        MapOverlay source = MapOverlay::OFF;
        std::string terrain;
        std::uint64_t lifetime = 0, surface = 0, vegetation = 0;
        int layer = -1;
        std::vector<MapOverlaySample> samples;
    } m_mapOverlayCache;
    std::string m_mapStatus;
    std::string m_scatterTargetId;
    char m_scatterSource[512] = {};
    char m_scatterDensityMap[512] = {};
    std::string m_scatterStatus;
    float m_scatterSpacing = 6.0f;
    float m_scatterDensity = 1.0f;
    int m_scatterSeed = 1;
    float m_scatterMinHeight = -100000.0f;
    float m_scatterMaxHeight = 100000.0f;
    float m_scatterMinSlope = 0.0f;
    float m_scatterMaxSlope = 40.0f;
    int m_scatterLayer = -1;
    float m_scatterLayerThreshold = 0.25f;
    float m_scatterMinScale = 0.8f;
    float m_scatterMaxScale = 1.2f;
    float m_scatterMinYaw = 0.0f;
    float m_scatterMaxYaw = 360.0f;
    int m_scatterOrientation = 0;
    float m_scatterDownslopeJitter = 0.0f;
    float m_scatterEmbedDepth = 0.0f;
    int m_scatterMaxInstances = 100000;
    int m_scatterMaxPlacementsPerCell = 0;
    /// @}

    /// @return GUID から解決した選択先。削除・無効化した対象を別の地形へ自動で置き換えない。
    scene::GameObject* DrawHeightMapTarget(scene::Scene& scene);
    void DrawImportSection(EditorContext& context, scene::GameObject* target);
    void DrawBiomeSection(EditorContext& context, scene::GameObject* target);
    void DrawTerrainMapsSection(EditorContext& context, scene::GameObject* target);
    bool DrawTerrainHabitat(scene::GameObject* target);
    void DrawNaturalTerrainSections(EditorContext& context, scene::GameObject* target, const OpArgs& mapArgs);
    void ClearFlowPreview();
    void CaptureFlowPreview(const OpData& data);
    void DrawFlowPreview(const renderer::Camera& camera, const ImVec2& origin, const ImVec2& size) const;
    void DrawTerrainMapOverlay(EditorContext& context, const renderer::Camera& camera, const ImVec2& origin, const ImVec2& size);
    void DrawTerrainRecipeSection(EditorContext& context, scene::GameObject* target);
    OpData CaptureTerrainRecipe(const scene::TerrainComponent* terrain) const;
    void RestoreTerrainRecipe(const OpData& recipe);
    void DrawGrassPaint(EditorContext& context, scene::GameObject* terrain);
    void DrawHeightMapPaint();
    void UpdateHeightMapStamp(EditorContext& context, bool pressed);
    void DrawHeightMapStampPreview(const ImVec2& viewportMin, const ImVec2& viewportSize, const renderer::Camera& camera) const;
    void DrawScatterSection(EditorContext& context, scene::GameObject* terrain);
    void DrawInstancePaint(EditorContext& context, scene::GameObject* terrain);
    void DrawInstanceWorkspace(EditorContext& context, scene::GameObject* terrain);
    void DrawInstanceAdjustments(EditorContext& context, scene::GameObject* terrain);
    void DrawPaintDiagnostics(const renderer::Camera& camera, const ImVec2& origin, const ImVec2& size) const;
    void DrawPaintDiagnosticSummary() const;
    void UpdateInstanceSelection(EditorContext& context, const renderer::Camera& camera, const ImVec2& origin, const ImVec2& size, bool hovered);
    void UpdatePaintArea(EditorContext& context, const renderer::Camera& camera, const ImVec2& origin, const ImVec2& size, bool pressed, bool hovered);
    void RefreshInstanceSelection(EditorContext& context);
    void PublishInstancePreview(EditorContext& context);
    void ApplyInstanceAdjustment(EditorContext& context, const std::string& action);
    void DrawScatterSettings(scene::GameObject* terrain);
    [[nodiscard]] OpArgs ScatterArguments(const std::string& terrainId, const std::string& targetId) const;
    void UpdateScatterStroke(EditorContext& context, bool pressed, bool held);
    void UpdateScatterPreview(EditorContext& context, bool viewportHovered);
    [[nodiscard]] OpArgs ScatterStrokeArguments(const std::string& terrainId,
        std::span<const math::Vector3> centers, float radius, bool erase) const;

    /// @brief DDA + 二分探索でレイと地形の交差を求める。穴のセルの交差は飛ばして次を探す。
    /// @param outLocalHit テレインローカル座標。
    bool RaycastSingleTerrain(const math::Ray&                 ray,
                               const scene::TerrainComponent&   terrain,
                               const scene::Transform&          tf,
                               math::Vector3&                   outLocalHit) const;

    /// @brief ストロークで初めて触れる地形の変更前を取っておく。
    TerrainStrokeSnapshot& CaptureStrokeBefore(scene::GameObject& go, const scene::TerrainComponent& terrain);
    void ClearStrokeSnapshots();
    /// @brief ストロークを確定し、触れた地形を 1 つの Undo にまとめる。
    /// @param rebuildCollider 形が変わる操作 (Sculpt / Ramp / Hole) なら true。
    void CommitStroke(EditorContext& context, const char* undoLabel, bool rebuildCollider);
    /// @brief Ramp の押下・ドラッグ・確定。
    void UpdateRamp(EditorContext& context, bool mousePressed, bool mouseReleased);

    /// @brief ブラシ円をビューポートに ImGui DrawList で描く (スクリーン空間投影)。
    void DrawBrushPreview(const ImVec2&           viewportMin,
                          const ImVec2&           viewportSize,
                          const renderer::Camera& camera) const;
    /// @brief Ramp の線分と幅をビューポートに描く。
    void DrawRampPreview(const ImVec2&           viewportMin,
                         const ImVec2&           viewportSize,
                         const renderer::Camera& camera) const;
};

} /// @note namespace fbzz::editor
