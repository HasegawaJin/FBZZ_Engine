/// @file    DebugCamera.cpp
/// @brief   Scene View 風デバッグカメラの入力処理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// マウス・キーボード入力を Camera の回転、パン、ドリーへ変換する。
/// エディタ操作向けの一時視点であり、ゲームカメラとは分ける。
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Input/Input.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>

namespace fbzz::renderer {

namespace {

// 縦画角の半分の tangent。正投影の「見えている縦幅」と焦点距離の換算に使う。
float HalfFovTan(float fovYDeg) {
    return std::tan(math::ToRad(fovYDeg) * 0.5f);
}

} // namespace

void DebugCamera::LookAt(const math::Vector3& target) {
    math::Vector3 forward = (target - camera.m_position).Normalized();
    m_pitch         = math::ToDeg(std::asin(-forward.y));
    m_yaw           = math::ToDeg(std::atan2(forward.x, forward.z));
    m_focusDistance = (target - camera.m_position).Length();
    m_pivot         = target;
    m_dollyVelocity = 0.0f;
    ApplyRotation();
    // 正投影は「引き」がそのまま near クリップの余裕なので、狙った点を中央へ置き直す。
    if (camera.m_projection == ProjectionMode::Orthographic)
        RepositionFromPivot();
}

void DebugCamera::Teleport(const math::Vector3& pos, const math::Quaternion& rot) {
    camera.m_position = pos;
    camera.m_rotation = rot;
    const math::Vector3 fwd = camera.GetForward();
    m_pitch         = math::ToDeg(std::asin(math::Clamp(-fwd.y, -1.0f, 1.0f)));
    m_yaw           = math::ToDeg(std::atan2(fwd.x, fwd.z));
    m_pivot         = pos + fwd * ViewDistance();
    m_dollyVelocity = 0.0f;
}

void DebugCamera::RepositionFromPivot() {
    camera.m_position = m_pivot - camera.GetForward() * ViewDistance();
}

void DebugCamera::SetProjection(ProjectionMode mode) {
    if (camera.m_projection == mode) return;

    // WHY 画角を引き継ぐか: 切り替えた瞬間に対象の大きさが変わると、
    //     どこを見ているのか分からなくなる。ピボット位置での見かけの縦幅を保つ。
    const float halfTan = HalfFovTan(camera.m_fovY);
    if (mode == ProjectionMode::Orthographic) {
        camera.m_orthoHeight = math::Max(0.01f, 2.0f * m_focusDistance * halfTan);
    } else if (halfTan > math::EPSILON) {
        m_focusDistance = math::Max(0.1f, camera.m_orthoHeight * 0.5f / halfTan);
    }

    camera.m_projection = mode;
    m_dollyVelocity     = 0.0f;
    RepositionFromPivot();
}

void DebugCamera::ApplyRotation() {
    const math::Vector3 worldUp = { 0.0f, 1.0f, 0.0f };
    auto yawQ   = math::Quaternion::FromAxisAngle(worldUp, math::ToRad(m_yaw));
    auto pitchQ = math::Quaternion::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, math::ToRad(m_pitch));
    camera.m_rotation = yawQ * pitchQ;
}

void DebugCamera::ApplyDolly(float dt) {
    // 正投影は前後移動で見え方が変わらないため、ホイールはズーム (m_orthoHeight) が
    // 受け持つ。ここでドリーを効かせると「動いているのに何も起きない」操作になる。
    if (camera.m_projection == ProjectionMode::Orthographic) {
        m_dollyVelocity = 0.0f;
        return;
    }
    if (std::abs(m_dollyVelocity) < 0.001f) {
        m_dollyVelocity = 0.0f;
        return;
    }
    const float dolly = m_dollyVelocity * dt;
    m_focusDistance   = math::Max(0.1f, m_focusDistance - dolly);
    camera.m_position = m_pivot - camera.GetForward() * m_focusDistance;
    m_dollyVelocity  *= std::exp(-scrollDamping * dt);
}

void DebugCamera::Update(float dt, bool viewportHovered) {
    using namespace fbzz::input;

    const math::Vector2 mouseDelta = Input::MouseDelta();
    const float         scroll     = Input::MouseScrollDelta();
    const bool          altHeld    = Input::KeyHeld(KeyCode::ALT);
    const bool          shiftHeld  = Input::KeyHeld(KeyCode::SHIFT);
    const bool          lmb        = Input::MouseButton(0);
    const bool          mmb        = Input::MouseButton(2);
    const bool          rmb        = Input::MouseButton(1);

    const math::Vector3 worldUp = { 0.0f, 1.0f, 0.0f };
    const bool          ortho   = camera.m_projection == ProjectionMode::Orthographic;

    if (viewportHovered) {
        if (ortho) {
            // 倍率で拡縮する。引き算にすると、寄るほど 1 ノッチの効きが荒くなる。
            camera.m_orthoHeight = math::Clamp(
                camera.m_orthoHeight * std::exp(-scroll * 0.15f), 0.01f, 100000.0f);
        } else {
            m_dollyVelocity += scroll * scrollSpeed * m_focusDistance;
        }
    }

    // パンの効き。正投影では焦点距離が画角と無関係になるので、見えている縦幅を基準にする。
    const float panRef = ortho ? camera.m_orthoHeight * 0.5f : m_focusDistance;

    // --- 中ドラッグ : オービット回転 / Shift+中 : パン ---
    if (mmb) {
        if (shiftHeld) {
            const float panScale = panRef * panSensitivity;
            camera.m_position -= camera.GetRight() * (mouseDelta.x * panScale);
            camera.m_position += worldUp            * (mouseDelta.y * panScale);
            m_pivot = camera.m_position + camera.GetForward() * ViewDistance();
        } else {
            m_yaw   += mouseDelta.x * mouseSens;
            m_pitch += mouseDelta.y * mouseSens;
            m_pitch  = math::Clamp(m_pitch, -89.0f, 89.0f);
            ApplyRotation();
            RepositionFromPivot();
        }
        ApplyDolly(dt);
        return;
    }

    // --- Alt + 左ドラッグ : オービット回転 (Unity 互換) ---
    if (altHeld && lmb) {
        m_yaw   += mouseDelta.x * mouseSens;
        m_pitch += mouseDelta.y * mouseSens;
        m_pitch  = math::Clamp(m_pitch, -89.0f, 89.0f);
        ApplyRotation();
        RepositionFromPivot();
        ApplyDolly(dt);
        return;
    }

    // --- 右ドラッグ : FPS ルック + フライ ---
    if (rmb) {
        m_yaw   += mouseDelta.x * mouseSens;
        m_pitch += mouseDelta.y * mouseSens;
        m_pitch  = math::Clamp(m_pitch, -89.0f, 89.0f);
        ApplyRotation();

        float speed = moveSpeed;
        if (shiftHeld) speed *= fastMultiplier;

        math::Vector3 move    = math::Vector3::ZERO;
        math::Vector3 forward = camera.GetForward();
        math::Vector3 right   = camera.GetRight();

        if (Input::KeyHeld(KeyCode::W)) move += forward;
        if (Input::KeyHeld(KeyCode::S)) move -= forward;
        if (Input::KeyHeld(KeyCode::D)) move += right;
        if (Input::KeyHeld(KeyCode::A)) move -= right;
        if (Input::KeyHeld(KeyCode::E)) move += worldUp;
        if (Input::KeyHeld(KeyCode::Q)) move -= worldUp;

        if (move.LengthSq() > math::EPSILON)
            camera.m_position += move.Normalized() * (speed * dt);

        m_pivot = camera.m_position + camera.GetForward() * ViewDistance();
        ApplyDolly(dt);
        return;
    }

    // --- フリー : ドリーのみ ---
    m_pivot = camera.m_position + camera.GetForward() * ViewDistance();
    ApplyDolly(dt);
}

} // namespace fbzz::renderer
