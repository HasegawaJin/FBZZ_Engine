/// @file    IKSolverComponent.hpp
/// @brief   複数種の IK ソルバーを順序付きで実行するチェーン設定。
/// @author  Hasegawa Jin
/// @date    2026-05-30
#pragma once

#include <Engine/Scene/References/Entity.hpp>
#include <Engine/Scene/References/ObjectReference.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @brief IKChain が使用する解法を指定する。
/// @note 解法ごとに Component/System を増やさず、同一キャラクター内の依存順を一元管理する。
enum class IKSolverType : uint8_t {
    TwoBone   = 0,
    FootPlace = 1,
    AimAt     = 2,
    FABRIK    = 3,
    HandPlace = 4,
    FullBodyBiped = 5,

};

/// @brief 1 つの IK 解法と、その入力およびランタイム状態を保持する。
struct IKChain {
    IKSolverType type    = IKSolverType::TwoBone; ///< このチェーンに適用する IK の解法。
    bool         enabled = true; ///< このチェーンを IK の評価対象にするかどうか。
    float        weight  = 1.0f; ///< 元のアニメーション姿勢へ IK の結果を混ぜる割合。
    int          order   = 0; ///< 複数チェーンを同じ姿勢へ反映する処理順。

    std::vector<std::string> boneNames; ///< TwoBone: [Root, Mid, Tip]。Spine: Base から Tip までの可変長チェーン。
    ObjectReference targetReference; ///< GUID が接続先の正本。Marker と通常 GO・骨 GO のいずれも参照できる。
    ObjectReference poleReference; ///< チェーンを曲げる方向を決めるポールのシーン参照。GUID が正本。
    EntityID targetEntity = EntityID::INVALID; ///< 以下の EntityID/GUID は互換 alias。名前は旧接続の移行と SDK のチェーン識別ラベルに使う。
    EntityID poleEntity   = EntityID::INVALID; ///< ポールの旧形式のシーン内エンティティ ID。新形式へ移す際に使う。
    std::string targetName; ///< 旧ターゲットを名前で移行し、SDK からチェーンを識別する名前。
    std::string targetGuid; ///< 旧形式で保存されたターゲットの GUID。
    std::string poleName; ///< 旧ポールを名前から移行するときに使う名前。
    std::string poleGuid; ///< 旧形式で保存されたポールの GUID。
    bool targetReferenceMigrationPending = true; ///< 保存しない一度だけの旧形式移行状態。新形式のキーがあるロードでは空 GUID も含め false にする。
    bool poleReferenceMigrationPending = true; ///< 旧ポール参照の一回限りの移行が未処理かどうか。ランタイム専用。
    math::Vector3 targetOffset = math::Vector3::ZERO; ///< ターゲットの world position へ加算する world 単位のオフセット。ターゲット scale は掛けない。
    float maxExtension = 0.98f; ///< 二骨チェーンの全長に対して許す伸展距離の割合。
    float softness     = 0.05f; ///< 二骨チェーンが伸び切る手前から到達距離を滑らかに抑える割合。
    float minBendAngleDegrees = 0.0f; ///< TwoBone/HandPlace の関節曲げ角。0 度が伸展、180 度が完全屈曲。
    float maxBendAngleDegrees = 175.0f; ///< TwoBone と HandPlace が許す関節の最大曲げ角 [度]。
    bool autoPole      = false; ///< 二骨チェーンの曲げ方向を自動で求めるかどうか。
    math::Vector3 autoPoleLocalDirection = math::Vector3::ZERO; ///< 自動ポールで優先する、所有エンティティのローカル方向。0 なら指定なし。

    /// @note HandPlace は TwoBone で手首位置を解いた後、ターゲット姿勢へ手首を追従させる。
    /// @note リグ固有の握り方向はターゲットのローカル回転オフセットとして分離する。
    math::Quaternion handRotationOffset = math::Quaternion::Identity(); ///< HandPlace でターゲット姿勢に掛ける手首のローカル回転オフセット。
    float handRotationWeight = 1.0f; ///< HandPlace で目標の回転へ手首姿勢を追従させる割合。

    /// @note FullBodyBiped は同一 Component 内の部位別 Solver を共有ポーズ上で反復実行する。
    /// @note 後段の補正で上流のゴールがずれるため、少数回の反復で相互依存を収束させる。
    int fullBodyIterations = 4; ///< 全身 IK で部位ごとの解決を繰り返す回数の上限。
    float fullBodyMaxRotationDegrees = 75.0f; ///< 全身 IK の回転補正に適用する角度上限 [度]。
    float fullBodyTolerance = 0.005f; ///< 全身 IK を収束と見なす目標位置の誤差の上限 [m]。

    bool  useAnimatorIKWeight = true; ///< FootPlace は標準 Humanoid 名から左右の脚を解決し、地形へ接地させる。
    float rayUpRatio          = 0.5f; ///< 接地レイの始点を上へ置く距離を、脚の長さに対する倍率で指定する。
    float rayDownRatio        = 1.2f; ///< 接地レイを下へ伸ばす距離を、脚の長さに対する倍率で指定する。
    float footSurfaceOffset   = 0.05f; ///< 接地位置に加えて足裏を地表から離す距離 [m]。
    float correctionDeadZone  = 0.025f; ///< この距離 [m] に満たない足の高さ補正を抑える。
    float maxCorrection       = 0.12f; ///< 接地 IK の足の高さの補正上限 [m]。
    /// @note FK 足裏と地面の距離、および左右の FK 足裏高の差で接地重みが 0 になる距離 (world m)。
    /// @note 段差では支持側の足を基準に低い地面へ追従できる。
    float footPlantDistance   = 0.06f; ///< 足が接地を失う高さのしきい値 [m]。
    float smoothTime          = 0.10f; ///< 接地補正を指数平滑化する時定数 [秒]。
    math::Vector3 footNormalAxis = math::Vector3::ZERO; ///< 接地法線へ合わせる足ボーンのローカル軸。0 は明示指定なし。
    bool     adjustHip = true; ///< 足が接地できるよう腰の上下補正も適用するかどうか。
    std::string hipBoneName = "Hips"; ///< 腰の上下補正を適用するスケルトン内のボーン名。
    /// @note 腰の地形追従率 [0,1]。距離は world m。0 の上限はその方向の補正を禁止する。
    /// @see Docs/design/foot-ik-tuning.md
    float hipWeight = 1.0f; ///< 足の接地に応じた腰の上下補正を適用する割合。
    float maxHipLowering = 0.10f; ///< 足の接地に合わせて腰を下げられる最大距離 [m]。
    float maxHipRaising = 0.0f; ///< 足の接地に合わせて腰を上げられる最大距離 [m]。
    float hipBendReserve = 0.0f; ///< FK の膝曲げを維持する既定値は 0 m。maxExtension から人工的な沈み込みを生成しない。
    bool useRagdollFootShape = true; ///< 同じ個体に Ragdoll があれば、その物理形状の底面と足裏姿勢を接地目標にする。

    /// @note Spine 自動傾斜ウェイト: FootPlace の足高さ差を地形傾斜メトリクスとして weight を自動変化させる。
    /// @note 平地では低 weight で FK を維持し、坂道では自動で weight を上げて脊椎補正を効かせる。
    bool  spineAutoWeight      = false; ///< 地形の傾斜に応じて脊椎補正の重みを変えるかどうか。
    float spineFlatWeight      = 0.05f; ///< 平地でのウェイト (lowestCorrection ≈ 0 のとき)
    float spineSlopeRampMeters = 0.10f; ///< この足高さ差 (m) で chain.weight に到達する

    math::Vector3 lookAtAxis   = math::Vector3::FORWARD; ///< LookAt のローカル軸、角度制限、時間応答を設定する。
    math::Vector3 lookAtUpAxis = math::Vector3::UP; ///< 注視時に上向きを合わせるボーンのローカル軸。
    float lookAtClampAngle = 90.0f; ///< 注視先へ回せる角度の上限 [度]。
    float lookAtSpeed      = 10.0f; ///< 注視方向の平滑化で目標姿勢へ追従する速さ。

    math::Quaternion lookAtSmoothedRotation = math::Quaternion::Identity(); ///< LookAt の時間平滑化状態。シーンへは保存しない。
    bool lookAtHasState = false; ///< 前フレームの注視姿勢を平滑化の履歴として使えるかどうか。

    float smoothedLeft           = 0.0f; ///< FootPlace の指数平滑化状態。シーンへは保存しない。
    float smoothedRight          = 0.0f; ///< 右足の上下補正の平滑化後の値 [m]。
    math::Vector3 smoothedLeftSupportOffset = math::Vector3::ZERO; ///< 物理接触の瞬断で足目標を FK へ跳ね戻さない。水平追従を smoothTime で平滑化する。
    math::Vector3 smoothedRightSupportOffset = math::Vector3::ZERO; ///< 右足の支持点に追従する平滑化済みのワールドオフセット [m]。
    math::Vector3 smoothedLeftSoleNormal = math::Vector3::UP; ///< 地形の三角形境界で足裏の目標姿勢が跳ばないよう、法線も smoothTime で平滑化する。
    math::Vector3 smoothedRightSoleNormal = math::Vector3::UP; ///< 右足裏を合わせる平滑化済みのワールド接地法線。
    float smoothedHip            = 0.0f; ///< このチェーンで平滑化した腰の上下補正 [m]。
    float leftPlantWeight        = 0.0f; ///< 左足を地面へ固定する現在の重み。
    float rightPlantWeight       = 0.0f; ///< 右足を地面へ固定する現在の重み。
    float smoothedTerrainOnlyHip = 0.0f; ///< 明示した hipBendReserve を除いた地形起伏のみの腰変位。Spine/LookAt が参照する。
    float runtimeHipTarget = 0.0f; ///< 制限を適用する前後の診断に使う、腰の上下補正の目標 [m]。
    bool runtimeHipLimited = false; ///< 今回の腰の上下補正が許容範囲の制限を受けたかどうか。
    bool runtimeLeftGrounded = false; ///< 今回の FootPlace の評価で左足が接地したかどうか。
    bool runtimeRightGrounded = false; ///< 今回の FootPlace の評価で右足が接地したかどうか。
    float runtimeAppliedHip = 0.0f; ///< このチェーンで実際に適用した腰の上下補正 [m]。
    bool runtimePhysicsFootShape = false; ///< 足の接地目標にラグドールの足の物理形状を使ったかどうか。
};

/// @brief キャラクターに属する IK チェーン群を実行順とともに所有する。
struct IKSolverComponent {
    bool enabled = true; ///< このコンポーネントのすべての IK チェーンを評価対象にするかどうか。
    std::vector<IKChain> chains; ///< このキャラクターに対して評価できる IK チェーンの設定と状態。

    uint64_t runtimeUpdateCount = 0; ///< IKSystem の実行経路を Inspector で確認するランタイム診断値。シーンへは保存しない。
    int runtimeSolvedChainCount = 0; ///< 今回の更新で実際に解いたチェーン数。
    float runtimeAnimatorWeight = 0.0f; ///< 今回の更新時に Animator の姿勢から受け取った IK 重み。
    bool runtimeLeftFootGrounded = false; ///< 今回の IK 更新で左足の接地を確認したかどうか。
    bool runtimeRightFootGrounded = false; ///< 今回の IK 更新で右足の接地を確認したかどうか。
    float runtimeHipOffset = 0.0f; ///< IK の結果として今回適用した腰の上下変位 [m]。
    bool runtimeSkinningUploaded = false; ///< IK 適用後のパレットを描画用バッファにアップロードしたかどうか。
    int runtimeFullBodyIterations = 0; ///< 今回の全身 IK で行った反復回数。
    float runtimeFullBodyError = 0.0f; ///< 今回の全身 IK の目標位置に対する残差 [m]。
    bool runtimeFullBodyConverged = false; ///< 今回の全身 IK の残差が収束条件を満たしたかどうか。

    const char* GetTypeName() const { return "IK Solver"; }

    /// @note IKChain は可変長配列なので Inspector 側で型別に直接描画する。
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
    }
};

}
