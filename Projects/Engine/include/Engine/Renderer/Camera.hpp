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

/// @brief 何も描かれていない画素の色。HDR レンダーターゲットのクリア値。
/// @note Forward / Deferred / CameraComponent の既定値 3 か所が同じ色でなければならない。
///       別々に書くと «描画パスによって色が違う» 壊れ方をする。
inline constexpr math::Vector4 kDefaultBackgroundColor = { 0.005f, 0.005f, 0.02f, 1.0f };

/// @brief 描画を始めるときにバッファをどう初期化するか。
enum class CameraClearMode : uint8_t {
    /// @brief カラーを backgroundColor で塗り、深度もリセットする。単独カメラの既定。
    SolidColor = 0,
    /// @brief カラーには触れず、深度だけリセットする。前に描かれた絵の上に重ねるためのモード。
    /// @note 深度リセットにより、後から描いたものは前の絵の奥行きに関係なく手前に出る
    ///       (一人称の武器が壁にめり込まないのはこれ)。
    /// @note «自分より前に誰かが描いている» ことが前提。単独カメラに指定すると、hdrRT は
    ///       フレーム間で使い回すため前フレームの絵が残像として残る。重ねる相手を用意すること。
    DepthOnly = 1,
};

enum class ProjectionMode : uint8_t {
    /// @brief 遠近感のある通常の射影。
    Perspective = 0,
    /// @brief 平行投影。奥行きで大きさが変わらないため、Front / Top / Side からの寸法合わせに使う。
    /// @note m_fovY の代わりに m_orthoHeight が画角を決める。
    Orthographic = 1,
};

/// @brief クリップ空間へ写す行列の深度を反転する (z' = w - z)。xy と w は変えない。
/// @note Reversed-Z の深度バッファへ、通常の Z で組まれた行列 (UI のキャンバス等) を描くときに使う。
///       NDC 深度 0 (最手前) は 1 へ、1 (最遠) は 0 へ移る。
[[nodiscard]] inline math::Matrix4 ReverseClipDepth(const math::Matrix4& toClip)
{
    math::Matrix4 result = toClip;
    for (int col = 0; col < 4; ++col)
        result.m[2][col] = toClip.m[3][col] - toClip.m[2][col];
    return result;
}

class Camera {
public:
    math::Matrix4 GetViewMatrix()       const;
    /// @brief CPU 側で使う射影。深度は near → 0、far → 1 (視錐台・レイ・スクリーン座標変換の前提)。
    math::Matrix4 GetProjectionMatrix() const;
    math::Matrix4 GetViewProjection()   const;
    /// @brief GPU へ送る射影。カメラ視点の深度バッファは Reversed-Z なので near → 1、far → 0。
    /// @note xy と w は GetProjectionMatrix と同じ。深度バッファ (RenderTargetDesc::reversedZ) へ
    ///       描く・読むシェーダーに渡す行列はすべてこちらから作る。
    /// @see https://docs.unity3d.com/ScriptReference/GL.GetGPUProjectionMatrix.html (Unity, GL.GetGPUProjectionMatrix)
    math::Matrix4 GetGpuProjectionMatrix() const;
    math::Matrix4 GetGpuViewProjection()   const;

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
    /// @brief Orthographic のときに映る縦幅 (ワールド単位)。横幅は m_aspect 倍。
    /// @note 半分でなく全体を持つのは、ビューポート上の「1 画面に何 m 入るか」がそのまま値になり
    ///       地形サイズやアリーナ直径と直接見比べられるため。
    float m_orthoHeight = 20.0f;

    /// @brief このカメラで描き始めるときの背景色。何も写らない画素に残る色。
    /// @note RGB は 1 を超えてよい (HDR ターゲットへそのまま入る)。m_clearMode が DepthOnly の
    ///       ときは使われない。空 (Sky) を描くシーンでは空が上書きするため見えない。
    math::Vector4 m_backgroundColor = kDefaultBackgroundColor;

    CameraClearMode m_clearMode = CameraClearMode::SolidColor;
};

} // namespace fbzz::renderer
