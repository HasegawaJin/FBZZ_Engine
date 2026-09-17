/// @file    SpringBoneComponent.hpp
/// @brief   髪・布・アクセサリをアニメーションに遅れて追従させる二次モーション設定
/// @author  Hasegawa Jin
/// @date    2026-08-25
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

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
    /// @brief 追従先のボーン名。空ならオーナー GameObject の Transform を基準にする。
    std::string boneName;
    SpringBoneColliderShape shape = SpringBoneColliderShape::Sphere;
    /// @brief 基準ボーンのローカル空間での中心 [m]。Capsule では線分の始点。
    math::Vector3 offset     = math::Vector3::ZERO;
    /// @brief Capsule の線分終点 (ローカル空間)。Sphere では使わない。
    math::Vector3 tailOffset = math::Vector3::ZERO;
    float radius = 0.05f;
    bool  enabled = true;
};

/// @brief チェーン内の 1 ボーンぶんの積分状態。シーンへは保存しない。
struct SpringBoneNodeState {
    int nodeIndex   = -1;
    /// @brief 同じチェーン内の親の添字。-1 なら親は FK 姿勢のまま (チェーンの根)。
    int parentState = -1;

    /// @brief 静止時に「先端」が向くボーンローカル方向。初期化時に 1 度だけ確定する。
    /// @note 毎フレーム取り直すと軸が揺れの結果へ追従し、戻ろうとする先が自分の現在位置になる。
    ///       バネが復元力を失って発散するため、確定は初期化時の 1 回だけ。
    math::Vector3 boneAxis   = math::Vector3::ZERO;
    float         boneLength = 0.0f;

    /// @brief 先端のワールド位置。Verlet 積分の現在値と前フレーム値。
    math::Vector3 currentTail = math::Vector3::ZERO;
    math::Vector3 prevTail    = math::Vector3::ZERO;

    /// @brief このフレームの確定ワールド姿勢。子ノードが親として読む。
    math::Vector3    simPosition = math::Vector3::ZERO;
    math::Quaternion simRotation = math::Quaternion::Identity();
};

/// @brief 1 本の揺れもの (根ボーンから下の枝すべて)。
struct SpringBoneChain {
    bool        enabled = true;
    /// @brief 揺らし始めるボーン。この配下は分岐していてもすべて対象になる。
    std::string rootBoneName;
    /// @brief 根から数えて何段まで揺らすか。0 で葉まで。
    int         maxDepth = 0;

    /// @brief 静止姿勢へ戻ろうとする強さ [0,1]。大きいほど硬く、アニメーションに速く追従する。
    float stiffness = 0.6f;
    /// @brief 速度の減衰 [0,1]。大きいほど早く止まる。
    float damping   = 0.4f;
    /// @brief 重力加速度の大きさ [m/s^2]。0 で無重力 (アニメーションの反動だけで揺れる)。
    float gravityPower     = 0.0f;
    math::Vector3 gravityDirection = { 0.0f, -1.0f, 0.0f };

    /// @brief 揺れボーン側の衝突半径 [m]。
    float radius = 0.02f;
    /// @brief 静止方向からの最大ふれ角 [degrees]。0 で無制限。
    float limitAngle = 60.0f;
    /// @brief 揺れの適用率 [0,1]。0 で完全に FK ポーズ。
    float weight = 1.0f;

    /// @brief 葉ボーンに与える仮想先端の長さ [m]。
    /// @note 揺れの回転は「ボーンから先端へのベクトル」で決まるが、葉ボーンには子が無く
    ///       先端が取れない。0 にすると葉だけ揺れずに硬く残る。
    float leafTailLength = 0.03f;

    std::vector<SpringBoneNodeState> nodes;
    /// @brief nodes を組んだときのスケルトン。差し替わったら組み直す。
    const asset::Skeleton* builtSkeleton = nullptr;
};

/// @brief キャラクターに属する揺れもの一式。AnimatorSystem / IKSystem の後段で解く。
struct SpringBoneComponent {
    bool enabled = true;
    /// @brief 編集モードでも積分するか。false なら Play 中だけ揺れる。
    bool simulateInEditor = true;

    /// @brief オーナーが 1 フレームでこの距離を超えて動いたら慣性を捨てる [m]。
    /// @note ワープや Play 開始直後の位置確定で、髪だけが元の場所から引き伸ばされるのを防ぐ。
    float teleportResetDistance = 1.0f;

    std::vector<SpringBoneChain>    chains;
    std::vector<SpringBoneCollider> colliders;

    math::Vector3 lastOwnerPosition    = math::Vector3::ZERO;
    bool          hasLastOwnerPosition = false;

    std::uint64_t runtimeUpdateCount        = 0;
    int           runtimeSimulatedBoneCount = 0;
    int           runtimeActiveColliderCount = 0;

    const char* GetTypeName() const { return "Spring Bone"; }

    /// @note chains / colliders は可変長配列のため Reflect では出さない。IKSolverComponent と
    ///       同じく Inspector 側が型別に描き、SceneSerializer が明示的に読み書きする。
    void Reflect(IReflector& r)
    {
        r.Field("enabled",               enabled);
        r.Field("simulateInEditor",      simulateInEditor);
        r.Field("teleportResetDistance", teleportResetDistance);
    }
};

} // namespace fbzz::scene
