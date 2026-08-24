// FBZZ Engine
// ViewportCommon.hpp | fbzz::editor
// ViewportPanel の分割ファイルで共有する描画・ピッキングヘルパー
#pragma once

#include <Editor/Panels/ViewportPanel.hpp>
#include "../../Tools/TerrainTool.hpp"
#include "../../Tools/WaterTool.hpp"
#include "../../Tools/DetailTool.hpp"
#include "../../Tools/FoliageTool.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
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
// ワールド座標をビューポート内スクリーン座標へ投影する。カメラ背面なら false。
bool WorldToScreen(const math::Vector3& world, const EditorContext& ctx,
                   const ImVec2& vpMin, const ImVec2& vpSize, ImVec2& out);
bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath);
bool InstantiateAssetAtViewport(EditorContext& ctx, const std::string& assetPath, const ImVec2& viewportMin);
bool PickEntity(EditorContext& ctx, const ImVec2& viewportMin);
// 選択を変えずにカーソル下の GameObject を返す (見つからなければ INVALID)。
scene::EntityID RaycastEntityAtMouse(EditorContext& ctx, const ImVec2& viewportMin);
// .mat ドラッグ中のホバープレビュー。カーソル下のオブジェクトへ仮適用し、
// 対象が変わったら前の対象を元へ戻す。ドロップ前の毎フレーム呼び出しを想定。
void UpdateMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state,
                               const std::string& materialPath, const ImVec2& viewportMin);
// 仮適用を破棄して元に戻す (ビューポート外へ出た / ドラッグがキャンセルされた場合)。
void CancelMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state);
// 仮適用をそのまま確定し、Undo を積む。何も適用していなければ false。
bool CommitMaterialDragPreview(EditorContext& ctx, MaterialDragPreviewState& state);
// 矩形 (ドラッグ) 選択: 投影位置が矩形内へ入る GameObject を選択へ加える。
// Ctrl なしは選択の置き換え、Ctrl ありは追加。
void RectSelectEntities(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize,
                        const ImVec2& rectA, const ImVec2& rectB);
void DrawCanvasEditorGuides(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
void PickUIEntity(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
bool DrawUIGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize, int& drag, ImVec2& dragStart, float& startX, float& startY, float& startWidth, float& startHeight, float& startAngle, float& startZ);
// 矢印キーで選択中 UI 要素を微移動する（1px / Shift で 10px）。allowed=false のときは何もしない。
void HandleUINudge(EditorContext& ctx, bool allowed);
void DrawSceneIcons(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize);
// Game ビューポート左下のフレーム / 描画統計 HUD。
// 位置は「呼び出し元ウィンドウの左下」を基準にするため、対象ビューポートの
// Begin/End スコープ内から呼ぶこと。
void DrawStatsOverlay(EditorContext& ctx);
// 右上のナビゲーションギズモ (Blender / Godot 風の軸ボール)。
void DrawOrientationGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
// ナビゲーションギズモがマウスを取っているか。DrawOrientationGizmo より前に問い合わせるため、
// 返る値は前フレームの状態 (ImGuizmo の ViewManipulate 判定と同じ扱い)。
bool IsOrientationGizmoHovered();
bool IsOrientationGizmoActive();
void DrawGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize, int& lastOp, int& lastMode, bool& prevOver, bool& prevUsing);
// 頂点スナップ (V ドラッグ) / 面スナップ (Ctrl+Shift ドラッグ)。
// true を返している間はスナップドラッグ中なので、呼び出し側はギズモ・ピッキング・
// 矩形選択をスキップすること (同じ左ドラッグを二重に解釈しないため)。
bool HandleViewportSnapping(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize);

} // namespace fbzz::editor
