/// @file    IKSolverComponent.hpp
/// @brief   複数種の IK ソルバーを順序付きで実行するチェーン設定。
/// @author  Hasegawa Jin
/// @date    2026-05-30
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

/// IKChain が使用する解法を指定する。
/// @note 解法ごとに Component/System を増やさず、同一キャラクター内の依存順を一元管理する。
enum class IKSolverType : uint8_t {
    TwoBone   = 0,
    FootPlace = 1,
    AimAt     = 2,
    FABRIK    = 3,
    HandPlace = 4,
    FullBodyBiped = 5,

};

/// 1 つの IK 解法と、その入力およびランタイム状態を保持する。
struct IKChain {
    IKSolverType type    = IKSolverType::TwoBone;
    bool         enabled = true;
    float        weight  = 1.0f;
    int          order   = 0;

    /// TwoBone: [Root, Mid, Tip]。Spine: Base から Tip までの可変長チェーン。
    std::vector<std::string> boneNames;
    EntityID targetEntity = EntityID::INVALID;
    EntityID poleEntity   = EntityID::INVALID;
    /// EntityID はロードごとに変わるため GUID を優先し、名前は編集時の表示にも使用する。
    std::string targetName;
    std::string targetGuid;
    std::string poleName;
    std::string poleGuid;
    math::Vector3 targetOffset = math::Vector3::ZERO;
    float maxExtension = 0.98f;
    float softness     = 0.05f;
    /// TwoBone/HandPlace の関節曲げ角。0 度が伸展、180 度が完全屈曲。
    float minBendAngleDegrees = 0.0f;
    float maxBendAngleDegrees = 175.0f;
    bool autoPole      = false;
    math::Vector3 autoPoleLocalDirection = math::Vector3::ZERO;

    /// HandPlace は TwoBone で手首位置を解いた後、ターゲット姿勢へ手首を追従させる。
    /// @note リグ固有の握り方向はターゲットのローカル回転オフセットとして分離する。
    math::Quaternion handRotationOffset = math::Quaternion::Identity();
    float handRotationWeight = 1.0f;

    /// FullBodyBiped は同一 Component 内の部位別 Solver を共有ポーズ上で反復実行する。
    /// @note 足・骨盤・背骨・腕を独立に一度だけ解くと後段の補正で上流のゴールがずれるため、
    ///       少数回の反復で相互依存を収束させる。
    int fullBodyIterations = 4;
    float fullBodyMaxRotationDegrees = 75.0f;
    float fullBodyTolerance = 0.005f;

    /// FootPlace は標準 Humanoid 名から左右の脚を解決し、地形へ接地させる。
    bool  useAnimatorIKWeight = true;
    float rayUpRatio          = 0.5f;
    float rayDownRatio        = 1.2f;
    float footSurfaceOffset   = 0.05f;
    float correctionDeadZone  = 0.025f;
    float maxCorrection       = 0.12f;
    /// 左右の足の地面クリアランス差がこれを超えた場合、高い側をスイング中とみなす。
    /// @note 足首ピボットの基準高はモデルごとに異なるため、地面からの絶対距離では判定しない。
    float footPlantDistance   = 0.06f;
    float smoothTime          = 0.10f;
    math::Vector3 footNormalAxis = math::Vector3::ZERO;
    bool     adjustHip = true;
    std::string hipBoneName = "Hips";

    /// Spine 自動傾斜ウェイト: FootPlace の足高さ差を地形傾斜メトリクスとして weight を自動変化させる。
    /// @note 平地では低 weight で FK を維持し、坂道では自動で weight を上げて脊椎補正を効かせる。
    bool  spineAutoWeight      = false;
    float spineFlatWeight      = 0.05f;  ///< 平地でのウェイト (lowestCorrection ≈ 0 のとき)
    float spineSlopeRampMeters = 0.10f;  ///< この足高さ差 (m) で chain.weight に到達する

    /// LookAt のローカル軸、角度制限、時間応答を設定する。
    math::Vector3 lookAtAxis   = math::Vector3::FORWARD;
    math::Vector3 lookAtUpAxis = math::Vector3::UP;
    float lookAtClampAngle = 90.0f;
    float lookAtSpeed      = 10.0f;

    /// LookAt の時間平滑化状態。シーンへは保存しない。
    math::Quaternion lookAtSmoothedRotation = math::Quaternion::Identity();
    bool lookAtHasState = false;

    /// FootPlace の指数平滑化状態。シーンへは保存しない。
    float smoothedLeft           = 0.0f;
    float smoothedRight          = 0.0f;
    float smoothedHip            = 0.0f;
    float leftPlantWeight        = 0.0f;
    float rightPlantWeight       = 0.0f;
    /// bendReserve を除いた地形起伏のみの Hip 変位。Spine/LookAt がこれを参照する。
    /// @note bendReserve は膝を曲げるための人工的なオフセットで平地でも非ゼロになるため、
    ///       smoothedHip をそのまま渡すと Spine IK が平地で意図せず傾いてしまう。
    float smoothedTerrainOnlyHip = 0.0f;
};

/// キャラクターに属する IK チェーン群を実行順とともに所有する。
struct IKSolverComponent {
    bool enabled = true;
    std::vector<IKChain> chains;

    /// IKSystem の実行経路を Inspector で確認するランタイム診断値。シーンへは保存しない。
    uint64_t runtimeUpdateCount = 0;
    int runtimeSolvedChainCount = 0;
    float runtimeAnimatorWeight = 0.0f;
    bool runtimeLeftFootGrounded = false;
    bool runtimeRightFootGrounded = false;
    float runtimeHipOffset = 0.0f;
    bool runtimeSkinningUploaded = false;
    int runtimeFullBodyIterations = 0;
    float runtimeFullBodyError = 0.0f;
    bool runtimeFullBodyConverged = false;

    const char* GetTypeName() const { return "IK Solver"; }

    /// @note IKChain は可変長配列なので Inspector 側で型別に直接描画する。
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
    }
};

} // namespace fbzz::scene
