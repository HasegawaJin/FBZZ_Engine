/// @file    ViewportCommon.hpp
/// @brief   ViewportPanel の分割ファイルで共有する描画・ピッキングヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include <Editor/Panels/ViewportPanel.hpp>
#include "../../Tools/TerrainTool.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Ray.hpp>
#include <Math/Vector4.hpp>
#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace fbzz::editor {

math::Matrix4 ToColumnMajor(const math::Matrix4& rowMajor);
math::Ray ScreenRayFromMouse(const EditorContext& ctx, const ImVec2& viewportMin);
/// @brief ワールド座標をビューポート内スクリーン座標へ投影する。
/// @return カメラ背面なら false。out は未変更。
bool WorldToScreen(const math::Vector3& world, const EditorContext& ctx,
                   const ImVec2& vpMin, const ImVec2& vpSize, ImVec2& out);
bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath);
bool InstantiateAssetAtViewport(EditorContext& ctx, const std::string& assetPath, const ImVec2& viewportMin);
bool PickEntity(EditorContext& ctx, const ImVec2& viewportMin);
/// @brief 選択を変えずにカーソル下の GameObject を返す。
/// @return 見つからなければ INVALID。
scene::EntityID RaycastEntityAtMouse(EditorContext& ctx, const ImVec2& viewportMin);
/// @brief .mat ドラッグ中のホバープレビュー。カーソル下へ仮適用し、対象が変わったら前の対象を戻す。
/// @note ドロップ前の毎フレーム呼び出しを想定。
void UpdateMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state,
                               const std::string& materialPath, const ImVec2& viewportMin);
/// @brief 仮適用を破棄して元に戻す (ビューポート外へ出た / ドラッグがキャンセルされた)。
void CancelMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state);
/// @brief 仮適用をそのまま確定し、Undo を積む。
/// @return 何も適用していなければ false。
bool CommitMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state);
/// @brief 投影位置 (ワールド原点) が矩形内へ入る GameObject を選択する。
/// @note Ctrl なしは選択の置き換え、Ctrl ありは追加。
void RectSelectEntities(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize,
                        const ImVec2& rectA, const ImVec2& rectB);
void DrawCanvasEditorGuides(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
void PickUIEntity(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
/// @brief 選択中の UI 要素すべての矩形を描く。
/// @pre ギズモより先に呼ぶ (ハンドルを上に出す)。
void DrawUISelectionOutlines(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
bool DrawUIGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize, int& drag, ImVec2& dragStart, float& startX, float& startY, float& startWidth, float& startHeight, float& startAngle, float& startZ);
/// @brief 矢印キーで選択中 UI 要素を微移動する (1px / Shift で 10px)。allowed=false なら何もしない。
void HandleUINudge(EditorContext& ctx, bool allowed);

/// @brief Scene View のアイコン種別の数。添字は Overlays の並びと SceneIconInstance::type に共通。
[[nodiscard]] int SceneIconTypeCount();
/// @return EditorContext::hiddenSceneIcons に入る保存キー。改名すると保存済みの非表示設定が外れる。
[[nodiscard]] const char* SceneIconTypeKey(int type);
[[nodiscard]] const char* SceneIconTypeLabel(int type);
[[nodiscard]] bool IsSceneIconTypeVisible(const EditorContext& ctx, int type);
void SetSceneIconTypeVisible(EditorContext& ctx, int type, bool visible);

/// @brief 画面上に置かれたアイコン 1 個。描画とピッキングが同じ値を使う。
struct SceneIconInstance {
    scene::EntityID id;
    int    type      = 0;
    ImVec2 screenPos{};
    float  scale     = 1.0f;  ///< 距離で縮めた倍率。当たり半径にも掛ける。
    float  alpha     = 1.0f;  ///< 距離フェードと非アクティブ減衰を掛けた不透明度 [0,1]。
    bool   active    = true;
};

/// @brief 表示すべきアイコンを投影して集める。
/// @note 同じ GameObject に複数種のアイコンが付くと横へずらして重ならないようにする。
/// @param out 毎回クリアされる。showSceneIcons が false なら空。
void CollectSceneIcons(const EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize,
                       std::vector<SceneIconInstance>& out);
/// @return スクリーン上の当たり半径 [px]。
[[nodiscard]] float SceneIconHitRadius(const SceneIconInstance& icon);

void DrawSceneIcons(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize);
/// @brief Game ビューポート左下のフレーム / 描画統計 HUD。
/// @pre 対象ビューポートの Begin/End スコープ内から呼ぶ (位置は呼び出し元ウィンドウの左下基準)。
void DrawStatsOverlay(EditorContext& ctx);
/// @brief 右上のナビゲーションギズモ (Blender / Godot 風の軸ボール)。
void DrawOrientationGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
/// @brief ナビゲーションギズモがマウスを取っているか。
/// @note DrawOrientationGizmo より前に問い合わせるため、返る値は前フレームの状態。
bool IsOrientationGizmoHovered();
bool IsOrientationGizmoActive();
void DrawGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize, int& lastOp, int& lastMode, bool& prevOver, bool& prevUsing);
/// @brief 頂点スナップ (V ドラッグ) / 面スナップ (Ctrl+Shift ドラッグ)。
/// @return スナップドラッグ中は true。呼び出し側はギズモ・ピッキング・矩形選択をスキップすること。
bool HandleViewportSnapping(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize);

} // namespace fbzz::editor
