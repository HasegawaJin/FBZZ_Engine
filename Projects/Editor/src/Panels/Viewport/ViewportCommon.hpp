// FBZZ Engine
// ViewportCommon.hpp | fbzz::editor
// ViewportPanel の分割ファイルで共有する描画・ピッキングヘルパー
#pragma once

#include <Editor/Panels/ViewportPanel.hpp>
#include "../../Tools/TerrainTool.hpp"
#include "../../Tools/WaterTool.hpp"
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
bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath);
bool InstantiatePrefabAssetAtViewport(EditorContext& ctx, const std::string& assetPath, const ImVec2& viewportMin);
void HandleGizmoShortcuts(EditorContext& ctx);
bool PickEntity(EditorContext& ctx, const ImVec2& viewportMin);
void DrawCanvasEditorGuides(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
void PickUIEntity(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
bool DrawUIGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize, int& drag, ImVec2& dragStart, float& startX, float& startY, float& startWidth, float& startHeight, float& startAngle, float& startZ);
void DrawSceneIcons(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize);
void DrawOrientationGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize);
void DrawGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize, int& lastOp, int& lastMode, bool& prevOver, bool& prevUsing);

} // namespace fbzz::editor
