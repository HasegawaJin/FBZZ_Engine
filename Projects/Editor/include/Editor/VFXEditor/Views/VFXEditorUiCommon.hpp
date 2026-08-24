// FBZZ Engine
// VFXEditorUiCommon.hpp | fbzz::editor
// VFX Editor の View 群で共有する ImGui ヘルパーと表示定数
// WHY: ノードの色・アイコン・要約表示や、パラメーター値エディタは Canvas / Inspector /
//      Timeline のどれからも使う。各 View の匿名 namespace に複製すると、
//      「Canvas では新しい色、Inspector では古い色」のような不整合が必ず生まれる。
// NOTE: ImGui に依存するのはこの View 層だけ。Document / Services からは参照しない。
#pragma once

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <imgui.h>
#include <cstddef>
#include <string>

namespace fbzz::renderer { class ResourceManager; class IImGuiRenderer; }

namespace fbzz::editor::vfx {

// ── 表示定数 ──
constexpr float kScrubStep     = 1.0f / 60.0f; // Step ボタン / スクラブの最小時間刻み
constexpr float kTimelineH     = 58.0f;        // タイムラインキャンバスの高さ [px]
constexpr float kBurstMarkerR  = 6.0f;         // Burst マーカー (菱形) の半径 [px]
// リンク失敗ノードを赤枠表示し続ける時間 [秒]。
constexpr float kErrorHighlightSeconds = 3.0f;

// VFX Editor 内部 DockSpace の各ウィンドウ名。Panel と各 View で同じ ID を指す必要がある。
constexpr const char* kWorkspaceWindow = "Graph###VFXWorkspace";
// 空間の親子 (parentNodeId) を木として見せ、D&D で組み替える面。Canvas とは別軸。
constexpr const char* kHierarchyWindow = "Hierarchy###VFXGraphHierarchy";
constexpr const char* kPreviewWindow   = "Preview###VFXPreview";
constexpr const char* kInspectorWindow = "Inspector###VFXInspector";
constexpr const char* kTimelineWindow  = "Timeline###VFXTimeline";

// よく使う拡張子フィルタ。ピッカーの候補を絞り、誤ったアセットの割り当てを減らす。
constexpr const char* kTextureExts  = ".png,.jpg,.jpeg,.tga,.dds,.bmp";
constexpr const char* kMaterialExts = ".mat";
constexpr const char* kMeshExts     = ".fbx,.obj,.mesh,.gltf,.glb";
constexpr const char* kAudioExts    = widgets::kAudioClipAssetFilter;
constexpr const char* kVfxExts      = ".vfx";
constexpr const char* kAnimatorControllerExts = ".animcontroller,.animctrl";

// Inspector のアセット欄で使う projectRoot。widgets::AssetPathField が必要とするが
// ヘルパー側は EditorContext を受け取らないため、描画開始時に一度だけ写す。
extern std::string g_assetFieldProjectRoot;

// ノードサムネイルの解決に使うレンダラー (非所有)。g_assetFieldProjectRoot と同じ理由で、
// ヘルパーが EditorContext を受け取らないため描画開始時に一度だけ写す。
// 未設定 (nullptr) のときサムネイルは単に出ない ― 描画が止まることはない。
extern renderer::ResourceManager*  g_thumbnailResources;
extern renderer::IImGuiRenderer*   g_thumbnailImGuiRenderer;

bool InputString(const char* label, std::string& value, std::size_t capacity = 512);
bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath);

// 手続き生成の Impact Decal を作る UI の入力値。Inspector の該当セクションが保持する。
struct ProceduralImpactDecalUiState {
    int textureSize = 512;
    int seed = 1;
    float radius = 0.72f;
    int crackCount = 12;
    float emissiveStrength = 0.65f;
    std::string status;
    bool statusIsError = false;
};

std::string ProceduralDecalOutputDirectory(const std::string& projectRoot);

// Scene Inspector と同じアセット欄。拡張子バッジ + ファイル名だけを表示し、
// クリックでフルパス編集、"..." でピッカー、ドラッグ&ドロップで割り当てできる。
bool AssetPathField(const char* label, std::string& value, const char* filterExts = nullptr);

// ImNodes の pin / link 識別子。ノード id から決定的に導出し、フレーム間で安定させる。
int InputPinId(int nodeId);
int OutputPinId(int nodeId);
int LinkId(int linkIndex);

const char* VFXNodeIcon(asset::VFXNodeType type);
ImU32 VFXNodeColor(asset::VFXNodeType type);

// ノード本体へ種別ごとの要点を出す (どのマテリアル・どの .vfx を使うか等)。
void DrawVFXNodeSummary(const asset::VFXGraphNode& node);

// ノードが代表する素材のパス (Particle なら material/texture、Decal なら albedo)。無ければ空。
[[nodiscard]] std::string VFXNodeThumbnailAsset(const asset::VFXGraphNode& node);

// ノード本体へ素材のサムネイルを出す。解決できない種別・パスでは何も描かない。
// WHY: 素材をパス文字列でしか出していないと、名前が似た素材の取り違えに気付けない。
//      ホバーで拡大するのは、粒子素材の要であるアルファの縁の勾配が 48px では見えないため。
void DrawVFXNodeThumbnail(const asset::VFXGraphNode& node);

// 公開パラメーターの値エディタ。Constant / Curve / Gradient / Random / Attribute / Signal を切り替える。
bool DrawVFXParamValue(const char* label, asset::VFXParamType type, asset::VFXParamValue& value,
                       float minimum, float maximum, bool hasRange);

} // namespace fbzz::editor::vfx
