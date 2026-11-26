/// @file    ClothSolver.hpp
/// @brief   ワールド空間の質点布を進める CPU XPBD ソルバー。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Physics/Api.hpp>

#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Matrix3.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace fbzz::physics {

struct ClothSettings {
    math::Vector3 gravity{0.0f, -9.81f, 0.0f}; ///< 布の質点に加えるワールド空間の重力加速度 [m/s²]。
    math::Vector3 windVelocity{}; ///< 追加の風サンプルを足す前のワールド空間の媒質速度 [m/s]。
    float stretchCompliance = 0.000001f; ///< 伸び拘束の柔らかさ [m/N]。0 で剛。
    float bendCompliance = 0.001f; ///< 曲げ拘束の柔らかさ。単位は距離方式なら [m/N]、二面角方式なら [1/(N·m)]。
    bool dihedralBending = false; ///< false は旧距離近似。true は符号付き二面角の XPBD、compliance の単位は 1/(N m)。
    float damping = 1.0f; ///< 布の速度を減衰させる係数。
    float airDensity = 1.225f; ///< 面に働く抗力を求める媒質密度 [kg/m³]。
    float dragCoefficient = 1.0f; ///< 布の面に働く空気抵抗の無次元係数。
    float thickness = 0.005f; ///< 布と障害物の最低離隔に加える厚さ [m]。
    float friction = 0.3f; ///< 布の接触で接線速度を減らす摩擦係数。
    float selfCollisionDistance = 0.0f; ///< 自己衝突の最小距離 [m]。0 は質点・面・辺の自己衝突をすべて無効にする。
    float selfCollisionStiffness = 1.0f; ///< 自己衝突の位置補正に適用する剛性係数。
    bool selfCollisionFaces = false; ///< true で質点–三角形と辺–辺の近接も selfCollisionDistance で分離する。質点球同士の判定に追加する。
    bool continuousCollision = false; ///< true で球・カプセルとの掃引接触と、selfCollisionFaces 有効時の質点–三角形・辺–辺の CCD を行う。
    int substeps = 8; ///< 1 回の Step を分割する刻みの数。
    int iterations = 2; ///< 各サブステップで拘束を解き直す回数。
};

enum class ClothContactType { SPHERE, CAPSULE, PLANE };

/// @brief 接触形状はワールド空間 [m]。平面は dot(x, normal) >= offset を許す。
/// @note capsule は a/b が軸の両端。velocity は接触面の並進速度 [m/s]。
struct ClothContact {
    ClothContactType type = ClothContactType::SPHERE; ///< 接触する相手の幾何形状。
    math::Vector3 a{}; ///< 球の中心、またはカプセル軸の始点のワールド位置 [m]。
    math::Vector3 b{}; ///< カプセル軸の終点のワールド位置 [m]。
    math::Vector3 normal{0.0f, 1.0f, 0.0f}; ///< 平面の許容領域を向くワールド法線。
    math::Vector3 velocity{}; ///< 接触形状全体のワールド並進速度 [m/s]。
    float radius = 0.5f; ///< 球またはカプセルの半径 [m]。
    float offset = 0.0f; ///< 平面方程式 dot(x, normal) の右辺 [m]。
    math::Vector3 angularVelocity{}; ///< 接触形状のワールド角速度 [rad/s]。
    math::Vector3 rotationCenter{}; ///< 接触点の回転速度と反作用を算出するワールド中心 [m]。
    float inverseMass = 0.0f; ///< 0 は動かない/運動を指定する形状。正値なら反作用を ContactResponses に返す。
    math::Matrix3 inverseInertia{}; ///< ワールド空間の対称半正定値逆慣性 [1/(kg m²)]。運動を指定する形状ではゼロを渡す。
};
/// @note 接触形状に加えるワールド空間の力積 [N s] と、rotationCenter 周りの角力積 [N m s]。
struct ClothContactResponse {
    math::Vector3 impulse{}; ///< 相手の接触形状へ返すワールド力積 [N·s]。
    math::Vector3 angularImpulse{}; ///< 相手の rotationCenter まわりへ返すワールド角力積 [N·m·s]。
};
/// @note ワールド空間の位置 [m] から追加の媒質速度 [m/s] を返す。Scene の型や場の形式を Physics へ持ち込まない。
using ClothWindSampler = std::function<math::Vector3(const math::Vector3&)>;
class ClothSolver;
struct ClothMotionConstraint;
struct ClothInteraction {
    ClothSolver* solver = nullptr; ///< この相互接触に参加する布の非所有参照。
    float distance = 0.0f; ///< 参加する布の間に確保する最小距離 [m]。
    uint32_t layer = 0; ///< 衝突フィルターで相手から見えるこの布の層。
    uint32_t mask = 0xffffffffu; ///< この布が衝突を許可する層のビット集合。
    std::span<const ClothMotionConstraint> motion; ///< 相互接触の後で復元する、この布の移動制限。呼び出し中だけ借りる。
    bool faces = false; ///< 相手との質点対に加えて、面・辺の近接も解くかどうか。
    bool continuous = false; ///< true は面・辺接触も有効化する。相手との組はどちらかの指定で有効となる。
};

/// @note 中心と半径はワールド空間 [m]。中心を substep 補間し、radius=0 は質量に関係なく完全固定する。
/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints 球内に移動を制限する motion constraint。
struct ClothMotionConstraint {
    uint32_t particle = 0; ///< この移動制限を掛ける布の質点番号。
    math::Vector3 previousCenter{}; ///< ステップ開始時点の制限球のワールド中心 [m]。
    math::Vector3 center{}; ///< ステップ終了時点の制限球のワールド中心 [m]。
    float radius = 0.0f; ///< 質点を許容する球の半径 [m]。0 なら中心へ固定する。
};

/// @brief Scene/RHI に依存しない布の状態と距離・曲げ拘束を所有する。
/// @see https://matthias-research.github.io/pages/publications/XPBD.pdf XPBD の式 (18)–(19)。
/// @see https://matthias-research.github.io/pages/publications/smallsteps.pdf substep による拘束の収束。
class ClothSolver {
public:
    /// @brief 初期位置とメッシュから質点および伸び・曲げ拘束を作る。
    /// @param[in] positions 各質点の初期ワールド位置 [m]。
    /// @param[in] indices 三角形ごとに 3 個ずつ並べた positions の添字。
    /// @param[in] inverseMasses positions と同順・同数の逆質量 [1/kg]。0 は固定点。
    /// @return 不正な面・添字・質量・非多様体辺なら false。失敗時は既存状態を維持する。
    [[nodiscard]] FBZZ_PHYSICS_API bool Initialize(std::span<const math::Vector3> positions,
                                  std::span<const uint32_t> indices,
                                  std::span<const float> inverseMasses);
    /// @brief 次の Step から使う布の設定を更新する。
    /// @param[in] settings 力・拘束・接触のパラメーター。自己衝突距離は 0 または 1e-6 ～ 100 [m]。
    /// @return 非有限値または範囲外なら false。設定を変更しない。
    [[nodiscard]] FBZZ_PHYSICS_API bool SetSettings(const ClothSettings& settings);
    /// @brief 固定点を次の Step で向かわせる位置を指定する。
    /// @param[in] vertex 初期化時の positions を基準にした、逆質量 0 の質点番号。
    /// @param[in] position 固定点の補間先のワールド位置 [m]。
    /// @return 固定点以外・範囲外・非有限値なら false。固定先は未変更。
    [[nodiscard]] FBZZ_PHYSICS_API bool SetPinTarget(uint32_t vertex, const math::Vector3& position);
    /// @brief 入力の力と拘束で布を固定更新 1 回ぶん進める。
    /// @param[in] dt 進める時間 (0, 0.1] [秒]。
    /// @param[in] contacts ワールド空間の接触形状。呼び出し中だけ参照する。
    /// @param[in] motion 質点ごとの移動制限。呼び出し中だけ参照し、接触より優先する。固定質点への正半径は拒否する。
    /// @param[in] wind 三角形重心で同期評価し、windVelocity に加える風速を返す関数。空なら追加なし。呼び出し中だけ参照する。
    /// @return 不正な入力、計算上限の超過、非有限の計算結果や風速で false。位置と速度は未変更で、接触反作用は空。
    /// @note 自己 CCD は substep 開始時に交差がない前提。既に交差した状態からは解かない。
    /// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf Bridson et al. 2002 の近接と CCD。
    [[nodiscard]] FBZZ_PHYSICS_API bool Step(float dt, std::span<const ClothContact> contacts = {},
                            std::span<const ClothMotionConstraint> motion = {}, const ClothWindSampler& wind = {});
    /// @brief 初期位置に戻し、速度と固定先を初期化する。
    FBZZ_PHYSICS_API void Reset();
    [[nodiscard]] const std::vector<math::Vector3>& Positions() const { return m_positions; }
    [[nodiscard]] const std::vector<math::Vector3>& Velocities() const { return m_velocities; }
    [[nodiscard]] const ClothSettings& Settings() const { return m_settings; }
    /// @note 直近の成功 Step の入力 contact と同順。Step 失敗・Reset・初期化成功で空になる。
    [[nodiscard]] const std::vector<ClothContactResponse>& ContactResponses() const { return m_contactResponses; }
    /// @brief 布同士の質点球と任意の面・辺接触を解き、最後に移動制限を適用する。
    /// @param[in,out] cloths 布ごとの衝突設定を参照し、各 solver の位置と速度を更新する。同じ solver は一度だけ指定する。
    /// @param[in] dt 各布の Step に渡した固定更新時間 [秒]。
    /// @pre 同じ dt で各布の Step が成功していること。
    /// @return 不正な入力や解法の失敗で false。全参加者の位置と速度を維持する。
    /// @note 相互 CCD は固定更新の開始→終了の直線軌跡を扱う。substep 間の曲線軌跡と初期交差の解消は保証しない。
    /// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#inter-collision Inter-Collision。
    [[nodiscard]] FBZZ_PHYSICS_API static bool SolveInterCollision(std::span<const ClothInteraction> cloths, float dt);

private:
    struct DistanceConstraint {
        uint32_t a = 0; ///< 布の距離拘束が結ぶ 1 番目の質点番号。
        uint32_t b = 0; ///< 布の距離拘束が結ぶ 2 番目の質点番号。
        float restLength = 0.0f; ///< 変形前の両質点の距離 [m]。
        float lambda = 0.0f; ///< 今のサブステップで蓄積した距離拘束の乗数。
        bool bending = false; ///< 伸び拘束と区別し、旧方式の曲げ剛性を使うかどうか。
    };
    FBZZ_PHYSICS_API void SolveDistances(float h);
    struct BendConstraint {
        std::array<uint32_t,4> v{}; ///< 隣接する 2 面の共有辺と対向頂点の質点番号。
        float restAngle = 0; ///< 変形前の 2 面間の符号付き二面角 [rad]。
        float lambda = 0; ///< 今のサブステップで蓄積した曲げ拘束の乗数。
    };
    FBZZ_PHYSICS_API void SolveBending(float h);
    FBZZ_PHYSICS_API bool ApplyWind(float h, const ClothWindSampler& wind);
    FBZZ_PHYSICS_API void SolveContacts(std::span<const ClothContact> contacts, float h);
    FBZZ_PHYSICS_API void SolveMotion(std::span<const ClothMotionConstraint> motion, float fraction, float h, bool updateVelocity);
    FBZZ_PHYSICS_API bool SolveSelfContacts();
    /// @note 開始位置 m_previous から現在位置までの掃引 AABB で候補を作る。候補数が上限を超えたら false。
    FBZZ_PHYSICS_API bool BuildPrimitivePairs(float inflation);
    FBZZ_PHYSICS_API void SolvePrimitiveProximity();
    /// @return 交差を補正したら true。
    FBZZ_PHYSICS_API bool SolvePrimitiveContinuous();
    struct SelfCell {
        std::array<int64_t, 3> cell{}; ///< 自己衝突の格子における整数セル座標。
        uint32_t particle = 0; ///< このセルの候補として記録した質点番号。
    };
    struct SweepBox {
        math::Vector3 lower{}; ///< 掃引範囲を包むワールド AABB の各軸の最小座標 [m]。
        math::Vector3 upper{}; ///< 掃引範囲を包むワールド AABB の各軸の最大座標 [m]。
        uint32_t index = 0; ///< この箱が包む質点、三角形または辺の番号。
    };
    /// @note edgeEdge=false は v[0] が質点、v[1..3] が三角形。true は v[0..1] と v[2..3] の辺。
    struct PrimitivePair {
        std::array<uint32_t, 4> v{}; ///< 判定対象の質点番号。質点と面、または 2 本の辺の並び。
        bool edgeEdge = false; ///< true なら 2 辺を、false なら質点と面を判定する。
    };
    ClothSettings m_settings; ///< 各サブステップの力・拘束・衝突で使う検証済みの布設定。
    std::vector<math::Vector3> m_positions; ///< 現在の各質点のワールド位置 [m]。
    std::vector<math::Vector3> m_restPositions; ///< 初期化時と Reset 時に復元するワールド位置 [m]。
    std::vector<math::Vector3> m_velocities; ///< 現在の各質点のワールド速度 [m/s]。
    std::vector<math::Vector3> m_previous; ///< サブステップの開始時点で保存するワールド位置 [m]。掃引と速度更新に使う。
    std::vector<math::Vector3> m_stepPositions; ///< Step 開始時のワールド位置。更新失敗時と相互 CCD で参照する。
    std::vector<math::Vector3> m_stepVelocities; ///< Step 開始時のワールド速度。更新に失敗したときに復元する。
    std::vector<math::Vector3> m_pinTargets; ///< 固定質点を配置するワールド位置 [m]。添字は質点番号。
    std::vector<float> m_inverseMasses; ///< 質点ごとの元の逆質量 [1/kg]。0 は固定質点。
    std::vector<float> m_stepInverseMasses; ///< 移動制限による固定も含めた今回のステップ用逆質量 [1/kg]。
    std::vector<unsigned char> m_motionUsed; ///< 移動制限の検証で、質点がすでに指定されたかを記録する作業配列。
    std::vector<SelfCell> m_selfCells; ///< 自己衝突の近傍検索に用いるセル座標と質点番号。
    std::vector<uint64_t> m_selfExcluded; ///< 拘束で直接結ばれ自己衝突判定から除く質点ペア。
    std::vector<std::array<uint32_t, 2>> m_edges; ///< 自己衝突判定に使う布の辺。要素は両端の質点番号。
    std::vector<SweepBox> m_pointBoxes; ///< 質点ごとの掃引 AABB。面との近接候補を抽出する。
    std::vector<SweepBox> m_triangleBoxes; ///< 三角形ごとの掃引 AABB。質点との近接候補を抽出する。
    std::vector<SweepBox> m_edgeBoxes; ///< 辺ごとの掃引 AABB。別の辺との近接候補を抽出する。
    std::vector<PrimitivePair> m_primitivePairs; ///< 近接または掃引の詳細判定へ進む質点–面、辺–辺の候補。
    std::vector<uint32_t> m_indices; ///< 布を構成する三角形の頂点番号。連続する 3 要素が 1 面。
    std::vector<DistanceConstraint> m_constraints; ///< 布の伸びと距離近似による曲げを解く拘束。
    std::vector<BendConstraint> m_bends; ///< 布の二面角による曲げを解く拘束。
    std::vector<ClothContactResponse> m_contactResponses; ///< 最後に成功した Step で相手へ返す力積。入力の接触形状と同順。
};

}
