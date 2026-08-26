/// @file    ScreenProjection.hpp
/// @brief   ワールド座標をメインカメラの画面 UV へ落とす
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// WHY 自前で解くか:
///   ScriptCameraProxy::WorldToScreenPoint は «自分の GameObject に付いた
///   CameraComponent» しか見ない。カメラに乗っていないマネージャーからは呼べない。
///   加えてあちらはクリップ w の符号を見ないため、カメラの背後にある点が画面の
///   反対側へ鏡像として返る。画面効果の中心に使うと、後ろで起きたことに前が反応する。
#pragma once

#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <cmath>

namespace sandbox::screenproj {

/// カメラの前にある点だけ true を返す。UV は左上原点で、Fullscreen.hlsli が
/// 全画面パスへ渡すものと同じ向き。画面の外へ出る点も投影するので、
/// 戻り値の UV が 0..1 に収まるとは限らない。
[[nodiscard]] inline bool WorldToUv(fbzz::scene::GameObject& cameraObject,
                                    const fbzz::scene::CameraComponent& camera,
                                    const fbzz::math::Vector3& world,
                                    fbzz::math::Vector2& outUv)
{
    using namespace fbzz::math;

    const Vector3 relative = world - cameraObject.transform.worldPosition;
    const float   depth    = Vector3::Dot(relative, cameraObject.transform.forward);
    // 真横 (depth = 0) も弾く。割ると UV が発散して、画面の端で暴れる。
    if (depth <= EPSILON) return false;

    const float tanHalfY = std::tan(ToRad(camera.fovY) * 0.5f);
    if (tanHalfY <= EPSILON) return false;
    const float tanHalfX = tanHalfY * Max(camera.aspectRatio, EPSILON);

    const float ndcX = Vector3::Dot(relative, cameraObject.transform.right) / (depth * tanHalfX);
    const float ndcY = Vector3::Dot(relative, cameraObject.transform.up)    / (depth * tanHalfY);

    outUv = { ndcX * 0.5f + 0.5f, 0.5f - ndcY * 0.5f };
    return true;
}

} // namespace sandbox::screenproj
