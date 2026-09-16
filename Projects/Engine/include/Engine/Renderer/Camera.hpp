/// @file    Camera.hpp
/// @brief   ビュー・プロジェクション行列とカメラ姿勢、および描画開始時の背景色。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#pragma once

#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>

namespace fbzz::renderer {

/// 何も描かれていない画素の色。HDR レンダーターゲットのクリア値。
///
/// WHY 定数を 1 つ置くか:
///   この値は「シーンの背景」そのもので、Forward / Deferred / CameraComponent の
///   既定値の 3 か所が同じ色でなければならない。別々に書くと、片方の経路だけ
///   背景が変わるという «描画パスによって色が違う» 壊れ方をする。
inline constexpr math::Vector4 kDefaultBackgroundColor = { 0.005f, 0.005f, 0.02f, 1.0f };

/// 描画を始めるときにバッファをどう初期化するか。
enum class CameraClearMode : uint8_t {
    /// カラーを backgroundColor で塗り、深度もリセットする。単独カメラの既定。
    SolidColor = 0,
    /// カラーには触れず、深度だけリセットする。
    ///
    /// 前に描かれた絵の «上に» 重ねるためのモード。深度がリセットされるので、
    /// 後から描いたものは前の絵の奥行きに関係なく手前に出る
    /// (一人称の武器が壁にめり込まないのはこれ)。
    ///
    /// NOTE: «自分より前に誰かが描いている» ことが前提のモード。現状このエンジンは
    ///       1 ビュー = 1 カメラで、hdrRT はフレーム間で使い回すため、単独カメラに
    ///       指定すると前フレームの絵が残って残像になる。重ねる相手を用意すること。
    DepthOnly = 1,
};

enum class ProjectionMode : uint8_t {
    /// 遠近感のある通常の射影。
    Perspective = 0,
    /// 平行投影。奥行きで大きさが変わらないため、Front / Top / Side からの
    /// 寸法合わせに使う。m_fovY の代わりに m_orthoHeight が画角を決める。
    Orthographic = 1,
};

class Camera {
public:
    math::Matrix4 GetViewMatrix()       const;
    math::Matrix4 GetProjectionMatrix() const;
    math::Matrix4 GetViewProjection()   const;

    math::Vector3 GetForward() const;
    math::Vector3 GetRight()   const;
    math::Vector3 GetUp()      const;

    void LookAt(const math::Vector3& target);

    math::Vector3    m_position = { 0.0f, 0.0f, -10.0f };
    math::Quaternion m_rotation;

    float m_fovY   = 60.0f;
    float m_aspect = 16.0f / 9.0f;
    float m_near   = 0.1f;
    float m_far    = 1000.0f;

    ProjectionMode m_projection = ProjectionMode::Perspective;
    /// Orthographic のときに映る縦幅 (ワールド単位)。横幅は m_aspect 倍。
    /// WHY 半分ではなく全体か: ビューポート上の「1 画面に何 m 入るか」がそのまま値になり、
    ///     地形サイズやアリーナ直径と直接見比べられる。
    float m_orthoHeight = 20.0f;

    /// このカメラで描き始めるときの背景色。何も写らない画素に残る色。
    /// 空 (Sky) を描くシーンでは空が上書きするため見えない。
    /// RGB は 1 を超えてよい (HDR ターゲットへそのまま入る)。
    /// m_clearMode が DepthOnly のときは使われない。
    math::Vector4 m_backgroundColor = kDefaultBackgroundColor;

    CameraClearMode m_clearMode = CameraClearMode::SolidColor;
};

} // namespace fbzz::renderer
