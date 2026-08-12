// FBZZ Engine
// VFXPreviewCamera.hpp | fbzz::editor::ai
// AI capture 用 VFX プレビューの視点を、注視点まわりの球面座標から組み立てる
//
// WHY: 埋め込み版 (EditorApp) と独立版 (VFXEditorApp) の両方が同じ規則で視点を作る必要がある。
//      片方だけ実装すると「同じ camera 引数なのに起動方法で絵が違う」ことになり、
//      AI の評価が再現しなくなる。角度の定義もここ 1 か所に閉じる。
#pragma once

#include <Editor/EditorContext.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::editor::ai {

// request.valid が false のときは fallback (操作用プレビューのカメラ) をそのまま返す。
// aspect は読み戻す RT の実サイズから渡す (要求サイズではない)。
[[nodiscard]] inline renderer::Camera MakeVFXPreviewCamera(
    const EditorContext::VFXAiPreviewCamera& request,
    const renderer::Camera& fallback, float aspect)
{
    renderer::Camera camera = fallback;
    if (aspect > 0.0f) camera.m_aspect = aspect;
    if (!request.valid) return camera;

    // yaw = 0 を「正面」= 注視点の -Z 側から +Z を向く、と定義する。
    // ビルボードのシルエット確認は yaw = 90 (真横)、回り込みは yaw を振る。
    constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
    const float yaw   = request.yaw * kDegreesToRadians;
    const float pitch = request.pitch * kDegreesToRadians;
    const float horizontal = std::cos(pitch) * request.distance;
    const math::Vector3 target{ request.targetX, request.targetY, request.targetZ };
    camera.m_position = {
        target.x - std::sin(yaw) * horizontal,
        target.y + std::sin(pitch) * request.distance,
        target.z - std::cos(yaw) * horizontal,
    };
    camera.m_fovY = request.fovY;
    // 遠距離スイープで注視点が near/far から外れないよう、距離に追従させる。
    camera.m_near = (std::max)(0.01f, request.distance * 0.01f);
    camera.m_far  = (std::max)(100.0f, request.distance * 20.0f);
    camera.LookAt(target);
    return camera;
}

} // namespace fbzz::editor::ai
