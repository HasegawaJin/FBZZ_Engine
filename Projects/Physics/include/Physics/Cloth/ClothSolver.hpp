/// @file    ClothSolver.hpp
/// @brief   ワールド空間の質点布を進める CPU XPBD ソルバー。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once

#include <Math/Vector3.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fbzz::physics {

struct ClothSettings {
    math::Vector3 gravity{0.0f, -9.81f, 0.0f};
    math::Vector3 windVelocity{};
    float stretchCompliance = 0.000001f;
    float bendCompliance = 0.001f;
    float damping = 1.0f;
    float airDensity = 1.225f;
    float dragCoefficient = 1.0f;
    float thickness = 0.005f;
    float friction = 0.3f;
    /// @note 自己衝突の最小距離 [m]。0 は質点・面・辺の自己衝突をすべて無効にする。
    float selfCollisionDistance = 0.0f;
    float selfCollisionStiffness = 1.0f;
    /// @note true で質点–三角形と辺–辺の近接も selfCollisionDistance で分離する。質点球同士の判定に追加する。
    bool selfCollisionFaces = false;
    /// @note true で球・カプセルとの掃引接触と、selfCollisionFaces 有効時の質点–三角形・辺–辺の CCD を行う。
    bool continuousCollision = false;
    int substeps = 8;
    int iterations = 2;
};

enum class ClothContactType { SPHERE, CAPSULE, PLANE };

/// @brief 接触形状はワールド空間 [m]。平面は dot(x, normal) >= offset を許す。
/// @note capsule は a/b が軸の両端。velocity は接触面の並進速度 [m/s]。
struct ClothContact {
    ClothContactType type = ClothContactType::SPHERE;
    math::Vector3 a{};
    math::Vector3 b{};
    math::Vector3 normal{0.0f, 1.0f, 0.0f};
    math::Vector3 velocity{};
    float radius = 0.5f;
    float offset = 0.0f;
};

/// @note 中心と半径はワールド空間 [m]。中心を substep 補間し、radius=0 は質量に関係なく完全固定する。
/// @see https://nvidiagameworks.github.io/PhysX/3.3/PhysXGuide/Manual/Cloth.html#motion-constraints 球内に移動を制限する motion constraint。
struct ClothMotionConstraint {
    uint32_t particle = 0;
    math::Vector3 previousCenter{};
    math::Vector3 center{};
    float radius = 0.0f;
};

/// @brief Scene/RHI に依存しない布の状態と距離拘束を所有する。
/// @see https://matthias-research.github.io/pages/publications/XPBD.pdf XPBD の式 (18)–(19)。
/// @see https://matthias-research.github.io/pages/publications/smallsteps.pdf substep による拘束の収束。
class ClothSolver {
public:
    /// @return 不正な面・添字・質量・非多様体辺なら false。失敗時は既存状態を維持する。
    /// @note indices は三角形の列、inverseMasses は頂点と同数 [1/kg]、0 は固定点。
    [[nodiscard]] bool Initialize(std::span<const math::Vector3> positions,
                                  std::span<const uint32_t> indices,
                                  std::span<const float> inverseMasses);
    /// @return 非有限値または範囲外なら false。設定を変更しない。
    [[nodiscard]] bool SetSettings(const ClothSettings& settings);
    /// @return 固定点以外・範囲外・非有限値なら false。
    [[nodiscard]] bool SetPinTarget(uint32_t vertex, const math::Vector3& position);
    /// @return 不正な刻み・接触・移動制約、重複質点、計算の非有限値なら false。位置・速度は未変更。
    /// @note dt は (0, 0.1]。span は呼び出し中だけ参照する。固定質点への正半径は拒否する。
    /// @note 移動制約は接触より優先するため、両立しない設定では接触面へ侵入し得る。
    /// @note 自己 CCD は substep 開始時に交差がない前提。既に交差した状態からは解かない。
    /// @see https://graphics.stanford.edu/papers/cloth-sig02/cloth.pdf Bridson et al. 2002 の近接と CCD。
    [[nodiscard]] bool Step(float dt, std::span<const ClothContact> contacts = {},
                            std::span<const ClothMotionConstraint> motion = {});
    /// @brief 初期位置に戻し、速度と固定先を初期化する。
    void Reset();
    [[nodiscard]] const std::vector<math::Vector3>& Positions() const { return m_positions; }
    [[nodiscard]] const std::vector<math::Vector3>& Velocities() const { return m_velocities; }
    [[nodiscard]] const ClothSettings& Settings() const { return m_settings; }

private:
    struct DistanceConstraint {
        uint32_t a = 0;
        uint32_t b = 0;
        float restLength = 0.0f;
        float lambda = 0.0f;
        bool bending = false;
    };
    void SolveDistances(float h);
    void ApplyWind(float h);
    void SolveContacts(std::span<const ClothContact> contacts, float h);
    void SolveMotion(std::span<const ClothMotionConstraint> motion, float fraction, float h, bool updateVelocity);
    bool SolveSelfContacts();
    /// @note 開始位置 m_previous から現在位置までの掃引 AABB で候補を作る。候補数が上限を超えたら false。
    bool BuildPrimitivePairs(float inflation);
    void SolvePrimitiveProximity();
    /// @return 交差を補正したら true。
    bool SolvePrimitiveContinuous();
    struct SelfCell {
        std::array<int64_t, 3> cell{};
        uint32_t particle = 0;
    };
    struct SweepBox {
        math::Vector3 lower{};
        math::Vector3 upper{};
        uint32_t index = 0;
    };
    /// @note edgeEdge=false は v[0] が質点、v[1..3] が三角形。true は v[0..1] と v[2..3] の辺。
    struct PrimitivePair {
        std::array<uint32_t, 4> v{};
        bool edgeEdge = false;
    };
    ClothSettings m_settings;
    std::vector<math::Vector3> m_positions;
    std::vector<math::Vector3> m_restPositions;
    std::vector<math::Vector3> m_velocities;
    std::vector<math::Vector3> m_previous;
    std::vector<math::Vector3> m_stepPositions;
    std::vector<math::Vector3> m_stepVelocities;
    std::vector<math::Vector3> m_pinTargets;
    std::vector<float> m_inverseMasses;
    std::vector<float> m_stepInverseMasses;
    std::vector<unsigned char> m_motionUsed;
    std::vector<SelfCell> m_selfCells;
    std::vector<uint64_t> m_selfExcluded;
    std::vector<std::array<uint32_t, 2>> m_edges;
    std::vector<SweepBox> m_pointBoxes;
    std::vector<SweepBox> m_triangleBoxes;
    std::vector<SweepBox> m_edgeBoxes;
    std::vector<PrimitivePair> m_primitivePairs;
    std::vector<uint32_t> m_indices;
    std::vector<DistanceConstraint> m_constraints;
};

}
