/// @file    ViewportCamera.hpp
/// @brief   Scene View カメラの向き・射影を外から変える共有ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// @note Scene View のカメラは DebugCamera が yaw/pitch/pivot/focusDistance を別に保持しているため、
///       renderer::Camera へ直接書くと乖離し次のオービットやパンで視点が飛ぶ。要求フラグ経由にし、
///       EditorApp が DebugCamera 側の内部状態ごと同期させる。
#pragma once

#include <Engine/Renderer/Camera.hpp>

namespace fbzz::editor {

struct EditorContext;

/// 6 面の軸ビュー。名前は「そのカメラから見える面」で、Blender / Unity と同じ向き。
enum class AxisView { Front, Back, Left, Right, Top, Bottom };

/// ピボット (注視点) と距離を保ったまま、視点の向きだけ yaw / pitch で決め直す。
/// 角度は度。pitch は真上・真下で yaw が復元できなくなるため内部で ±89.9 に丸める。
void PointEditorCamera(EditorContext& ctx, float yawDeg, float pitchDeg);

/// 6 面ビューへ切り替える。射影は変えない (正投影と組み合わせるかは呼び出し側の判断)。
void SetEditorCameraAxisView(EditorContext& ctx, AxisView view);

/// 遠近 / 平行投影を切り替える。ピボット位置での見かけの大きさは引き継がれる。
void SetEditorCameraProjection(EditorContext& ctx, renderer::ProjectionMode mode);

/// 現在 Scene View が平行投影か。カメラ未設定なら false。
[[nodiscard]] bool IsEditorCameraOrthographic(const EditorContext& ctx);

} // namespace fbzz::editor
