/// @file    ViewportCamera.cpp
/// @brief   Scene View カメラの向き・射影を変える共有ヘルパーの実装。
/// @author  Hasegawa Jin
/// @date    2026-08-29
#include <Editor/Util/ViewportCamera.hpp>
#include <Editor/EditorContext.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::editor {

void PointEditorCamera(EditorContext& ctx, float yawDeg, float pitchDeg)
{
    if (!ctx.editorCamera) return;

    /// @note ±90 ちょうどにすると forward が真上 / 真下になり、Teleport 側の atan2(0, 0) から
    ///       yaw を復元できず 0 に落ちる。見た目には真上・真下と区別が付かない角度で止める。
    pitchDeg = math::Clamp(pitchDeg, -89.9f, 89.9f);

    const math::Quaternion yawQ =
        math::Quaternion::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, math::ToRad(yawDeg));
    const math::Quaternion pitchQ =
        math::Quaternion::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, math::ToRad(pitchDeg));
    const math::Quaternion rot = yawQ * pitchQ;
    const math::Vector3    fwd = rot * math::Vector3::FORWARD;

    ctx.teleportRotation      = rot;
    ctx.teleportPosition      = ctx.editorCameraPivot - fwd * ctx.editorCameraViewDistance;
    ctx.requestTeleportCamera = true;
}

void SetEditorCameraAxisView(EditorContext& ctx, AxisView view)
{
    /// @note yaw は「forward を +Z から時計回りに何度回すか」(atan2(fwd.x, fwd.z))。
    switch (view) {
    /// @note -Z から +Z を見る
    case AxisView::Front:  PointEditorCamera(ctx,    0.0f,   0.0f); break;
    case AxisView::Back:   PointEditorCamera(ctx,  180.0f,   0.0f); break;
    /// @note -X から +X を見る
    case AxisView::Left:   PointEditorCamera(ctx,   90.0f,   0.0f); break;
    case AxisView::Right:  PointEditorCamera(ctx,  -90.0f,   0.0f); break;
    /// @note 真上 / 真下は yaw を 0 に固定する。直前の yaw を残すと、押すたびに
    ///       平面図の «北» が変わって寸法を読み違える。
    case AxisView::Top:    PointEditorCamera(ctx,    0.0f,  90.0f); break;
    case AxisView::Bottom: PointEditorCamera(ctx,    0.0f, -90.0f); break;
    }
}

void SetEditorCameraProjection(EditorContext& ctx, renderer::ProjectionMode mode)
{
    ctx.cameraProjection        = mode;
    ctx.requestCameraProjection = true;
}

bool IsEditorCameraOrthographic(const EditorContext& ctx)
{
    return ctx.editorCamera
        && ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic;
}

} // namespace fbzz::editor
