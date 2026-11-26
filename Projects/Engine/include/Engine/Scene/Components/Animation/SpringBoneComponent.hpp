/// @file    SpringBoneComponent.hpp
/// @brief   髪・布・アクセサリをアニメーションに遅れて追従させる二次モーション設定
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once

#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset { struct Skeleton; }

namespace fbzz::scene {

/// @brief 揺れボーンが貫通できない形状。
enum class SpringBoneColliderShape : std::uint8_t {
    Sphere  = 0,
    Capsule = 1,
};

/// @brief 1 個の衝突形状。基準ボーンのワールド姿勢へ乗せて配置する。
struct SpringBoneCollider {
    std::string boneName; ///< 追従先のボーン名。空ならオーナー GameObject の Transform を基準にする。
    SpringBoneColliderShape shape = SpringBoneColliderShape::Sphere; ///< 揺れボーンを押し戻す衝突形状の種類。
    math::Vector3 offset     = math::Vector3::ZERO; ///< 基準ボーンのローカル空間での中心 [m]。Capsule では線分の始点。
    math::Vector3 tailOffset = math::Vector3::ZERO; ///< Capsule の線分終点 (ローカル空間)。Sphere では使わない。
    float radius = 0.05f; ///< 衝突形状の半径 [m]。球の中心またはカプセル軸から測る。
    bool  enabled = true; ///< この形状を揺れボーンとの衝突判定に含めるかどうか。
};

/// @brief チェーン内の 1 ボーンぶんの積分状態。シーンへは保存しない。
struct SpringBoneNodeState {
    int nodeIndex   = -1; ///< 積分状態を対応付けるスケルトンのノード添字。
    int parentState = -1; ///< 同じチェーン内の親の添字。-1 なら親は FK 姿勢のまま (チェーンの根)。

    /// @brief 静止時に「先端」が向くボーンローカル方向。初期化時に 1 度だけ確定する。
    /// @note 毎フレーム取り直すと軸が揺れの結果へ追従し、戻ろうとする先が自分の現在位置になる。
    /// @note バネが復元力を失って発散するため、確定は初期化時の 1 回だけ。
    math::Vector3 boneAxis   = math::Vector3::ZERO; ///< 初期化時に確定した、静止時の先端へ向くボーンローカルの軸。
    float         boneLength = 0.0f; ///< ボーン根元から計算上の先端までの距離 [m]。

    math::Vector3 currentTail = math::Vector3::ZERO; ///< 先端のワールド位置。Verlet 積分の現在値と前フレーム値。
    math::Vector3 prevTail    = math::Vector3::ZERO; ///< 速度を復元するために保存した、前の積分時点での先端のワールド位置 [m]。

    math::Vector3    simPosition = math::Vector3::ZERO; ///< このフレームの確定ワールド姿勢。子ノードが親として読む。
    math::Quaternion simRotation = math::Quaternion::Identity(); ///< 子ボーンが基準にする、揺れ計算後のワールド姿勢。
};

/// @brief 1 本の揺れもの (根ボーンから下の枝すべて)。
struct SpringBoneChain {
    bool        enabled = true; ///< このチェーンの揺れを積分して姿勢へ反映するかどうか。
    std::string rootBoneName; ///< 揺らし始めるボーン。この配下は分岐していてもすべて対象になる。
    int         maxDepth = 0; ///< 根から数えて何段まで揺らすか。0 で葉まで。

    float stiffness = 0.6f; ///< 静止姿勢へ戻ろうとする強さ [0,1]。大きいほど硬く、アニメーションに速く追従する。
    float damping   = 0.4f; ///< 速度の減衰 [0,1]。大きいほど早く止まる。
    float gravityPower     = 0.0f; ///< 重力加速度の大きさ [m/s^2]。0 で無重力 (アニメーションの反動だけで揺れる)。
    math::Vector3 gravityDirection = { 0.0f, -1.0f, 0.0f }; ///< 揺れものに加える重力のワールド方向。

    float radius = 0.02f; ///< 揺れボーン側の衝突半径 [m]。
    float limitAngle = 60.0f; ///< 静止方向からの最大ふれ角 [degrees]。0 で無制限。
    float weight = 1.0f; ///< 揺れの適用率 [0,1]。0 で完全に FK ポーズ。

    /// @brief 葉ボーンに与える仮想先端の長さ [m]。
    /// @note 揺れの回転は「ボーンから先端へのベクトル」で決まるが、葉ボーンには子が無く
    /// @note 先端が取れない。0 にすると葉だけ揺れずに硬く残る。
    float leafTailLength = 0.03f; ///< 子のないボーンに持たせる仮想先端の長さ [m]。0 は葉の揺れなし。

    std::vector<SpringBoneNodeState> nodes; ///< チェーンに含まれるボーンの順序付き積分状態。
    const asset::Skeleton* builtSkeleton = nullptr; ///< nodes を組んだときのスケルトン。差し替わったら組み直す。
};

/// @brief キャラクターに属する揺れもの一式。AnimatorSystem / IKSystem の後段で解く。
struct SpringBoneComponent {
    bool enabled = true; ///< このコンポーネントの揺れの更新を行うかどうか。
    bool simulateInEditor = true; ///< 編集モードでも積分するか。false なら Play 中だけ揺れる。

    /// @brief オーナーが 1 フレームでこの距離を超えて動いたら慣性を捨てる [m]。
    /// @note ワープや Play 開始直後の位置確定で、髪だけが元の場所から引き伸ばされるのを防ぐ。
    float teleportResetDistance = 1.0f; ///< オーナーの 1 フレームの移動で揺れの慣性をリセットする距離 [m]。

    std::vector<SpringBoneChain>    chains; ///< キャラクターに属する揺れボーンチェーンの設定と状態。
    std::vector<SpringBoneCollider> colliders; ///< 揺れボーンが貫通しないようにするための衝突形状の一覧。

    math::Vector3 lastOwnerPosition    = math::Vector3::ZERO; ///< ワープや急な移動を検知するために保存した前回のオーナーのワールド位置 [m]。
    bool          hasLastOwnerPosition = false; ///< ワープの判定に使える前回のオーナー位置を保持しているかどうか。

    std::uint64_t runtimeUpdateCount        = 0; ///< 揺れ計算の更新が行われた回数。
    int           runtimeSimulatedBoneCount = 0; ///< 今回の更新で揺れを積分したボーン数。
    int           runtimeActiveColliderCount = 0; ///< 今回の更新で衝突に使った有効な形状数。

    const char* GetTypeName() const { return "Spring Bone"; }

    /// @note chains / colliders は可変長配列のため Reflect では出さない。IKSolverComponent と
    /// @note 同じく Inspector 側が型別に描き、SceneSerializer が明示的に読み書きする。
    void Reflect(IReflector& r)
    {
        r.Field("enabled",               enabled);
        r.Field("simulateInEditor",      simulateInEditor);
        r.Field("teleportResetDistance", teleportResetDistance);
    }
};

}
