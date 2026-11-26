/// @file    ClothSolver.cpp
/// @brief   布ソルバーの初期化、入力検証、サブステップの進行と巻き戻し。
/// @author  Hasegawa Jin
/// @date    2026-09-17

#include <Physics/Cloth/ClothSolver.hpp>
#include "ClothSolverDetail.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace fbzz::physics {

namespace {

using math::Vector3;

using cloth_detail::GEOMETRY_EPSILON;
using cloth_detail::MAX_COLLISION_DISTANCE;
using cloth_detail::MIN_COLLISION_DISTANCE;

using cloth_detail::IsFiniteVector;
using cloth_detail::IsFiniteNonnegative;
using cloth_detail::ParticlePairKey;
using cloth_detail::ComputeDihedral;

bool ValidContact(const ClothContact& contact)
{
    // 接触位置と接触面の速度・反作用の計算へ非有限値を渡さない。
    if (!IsFiniteVector(contact.a) || !IsFiniteVector(contact.b) || !IsFiniteVector(contact.normal)) {
        return false;
    }
    if (!IsFiniteVector(contact.velocity) || !IsFiniteVector(contact.angularVelocity)) {
        return false;
    }
    if (!IsFiniteVector(contact.rotationCenter) || !IsFiniteNonnegative(contact.inverseMass)) {
        return false;
    }
    if (!IsFiniteNonnegative(contact.radius) || !std::isfinite(contact.offset)) {
        return false;
    }
    for (const auto& row : contact.inverseInertia.m) {
        for (float value : row) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
    }

    // カプセル軸の長さは有限である必要があり、平面法線は単位長にそろえて使う。
    switch (contact.type) {
    case ClothContactType::SPHERE:
        return true;
    case ClothContactType::CAPSULE:
        return std::isfinite((contact.b - contact.a).LengthSq());
    case ClothContactType::PLANE:
        return std::abs(contact.normal.LengthSq() - 1.0f) < 0.001f;
    default:
        return false;
    }
}

}

bool ClothSolver::Initialize(std::span<const math::Vector3> positions,
                             std::span<const uint32_t> indices,
                             std::span<const float> inverseMasses)
{
    // 頂点と質量の対応を先に確かめ、以降は三角形ごとに 3 頂点を読み出せるようにする。
    if (positions.empty() || indices.empty() || indices.size() % 3 != 0
        || inverseMasses.size() != positions.size()) {
        return false;
    }
    for (size_t i = 0; i < positions.size(); ++i) {
        if (!IsFiniteVector(positions[i]) || !IsFiniteNonnegative(inverseMasses[i])) {
            return false;
        }
    }
    struct Edge {
        uint32_t opposite; ///< この辺と同じ三角形に含まれる対向頂点の質点番号。
        bool paired = false; ///< この辺を共有する隣の三角形を既に見つけたかどうか。
    };
    std::map<std::pair<uint32_t, uint32_t>, Edge> edges;
    std::vector<DistanceConstraint> constraints;
    std::vector<BendConstraint> bends;
    std::vector<std::array<uint32_t, 2>> meshEdges;

    // 各辺に伸び拘束を作り、2 面で共有される辺には曲げ拘束も作る。
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= positions.size() || b >= positions.size() || c >= positions.size()) {
            return false;
        }
        const float areaSquared = Vector3::Cross(positions[b] - positions[a], positions[c] - positions[a]).LengthSq();
        if (!std::isfinite(areaSquared) || areaSquared < GEOMETRY_EPSILON * GEOMETRY_EPSILON) {
            return false;
        }
        const uint32_t triangle[]{a, b, c};
        for (int j = 0; j < 3; ++j) {
            const uint32_t x = triangle[j], y = triangle[(j + 1) % 3], opposite = triangle[(j + 2) % 3];
            const auto key = std::make_pair(std::min(x, y), std::max(x, y));
            auto [it, inserted] = edges.emplace(key, Edge{opposite});
            if (inserted) {
                const float length = (positions[x] - positions[y]).Length();
                if (!std::isfinite(length) || length <= GEOMETRY_EPSILON) {
                    return false;
                }
                constraints.push_back({x, y, length, 0.0f, false});
                meshEdges.push_back({key.first, key.second});
            } else {
                if (it->second.paired || it->second.opposite == opposite) {
                    return false;
                }
                it->second.paired = true;
                const float length = (positions[opposite] - positions[it->second.opposite]).Length();
                if (!std::isfinite(length) || length <= GEOMETRY_EPSILON) {
                    return false;
                }
                // 初期版の曲げは対頂点間距離による近似。二面角拘束とは異なる。
                constraints.push_back({opposite, it->second.opposite, length, 0.0f, true});
                BendConstraint bend;
                bend.v = {opposite,it->second.opposite,key.first,key.second};
                std::array<Vector3,4> p, gradient;
                for (size_t k = 0; k < 4; ++k) {
                    p[k] = positions[bend.v[k]];
                }
                if (!ComputeDihedral(p,bend.restAngle,gradient)) {
                    return false;
                }
                bends.push_back(bend);
            }
        }
    }

    // 入力全体の検証後に、拘束と初期状態をまとめて置き換える。
    m_positions.assign(positions.begin(), positions.end());
    m_restPositions = m_positions;
    m_pinTargets = m_positions;
    m_previous = m_positions;
    m_stepPositions = m_positions;
    m_velocities.assign(positions.size(), {});
    m_stepVelocities = m_velocities;
    m_inverseMasses.assign(inverseMasses.begin(), inverseMasses.end());
    m_indices.assign(indices.begin(), indices.end());
    m_constraints = std::move(constraints);
    m_bends = std::move(bends);
    m_contactResponses.clear();
    m_stepInverseMasses.clear();
    m_edges = std::move(meshEdges);
    m_primitivePairs.clear();
    m_selfExcluded.clear();

    // 拘束で結んだ質点対を、離隔補正の候補から二分探索で除外できるようにする。
    for (const auto& constraint : m_constraints) {
        m_selfExcluded.push_back(ParticlePairKey(constraint.a, constraint.b));
    }
    std::sort(m_selfExcluded.begin(), m_selfExcluded.end());
    return true;
}

bool ClothSolver::SetSettings(const ClothSettings& settings)
{
    // 力の積分と拘束に使う値を検証する。柔らかさや抵抗の係数は 0 を許す。
    if (!IsFiniteVector(settings.gravity) || !IsFiniteVector(settings.windVelocity)) {
        return false;
    }
    if (!IsFiniteNonnegative(settings.stretchCompliance) || !IsFiniteNonnegative(settings.bendCompliance)) {
        return false;
    }
    if (!IsFiniteNonnegative(settings.damping) || !IsFiniteNonnegative(settings.airDensity)
        || !IsFiniteNonnegative(settings.dragCoefficient)) {
        return false;
    }

    // 接触の厚みと摩擦を確かめ、自己衝突の離隔距離は 0 または格子で使える範囲に限る。
    if (!IsFiniteNonnegative(settings.thickness)) {
        return false;
    }
    if (!IsFiniteNonnegative(settings.friction) || settings.friction > 1.0f) {
        return false;
    }
    const float selfCollisionDistance = settings.selfCollisionDistance;
    if (!IsFiniteNonnegative(selfCollisionDistance)) {
        return false;
    }
    const bool belowMinimumDistance = selfCollisionDistance > 0.0f && selfCollisionDistance < MIN_COLLISION_DISTANCE;
    if (belowMinimumDistance || selfCollisionDistance > MAX_COLLISION_DISTANCE) {
        return false;
    }
    if (!IsFiniteNonnegative(settings.selfCollisionStiffness) || settings.selfCollisionStiffness > 1.0f) {
        return false;
    }

    // 時間刻みと反復の上限を確かめ、すべて検証できた設定だけを採用する。
    if (settings.substeps < 1 || settings.substeps > 64) {
        return false;
    }
    if (settings.iterations < 1 || settings.iterations > 32) {
        return false;
    }

    m_settings = settings;
    return true;
}

bool ClothSolver::SetPinTarget(uint32_t vertex, const math::Vector3& position)
{
    if (vertex >= m_positions.size() || m_inverseMasses[vertex] != 0.0f || !IsFiniteVector(position)) {
        return false;
    }
    m_pinTargets[vertex] = position;
    return true;
}

void ClothSolver::Reset()
{
    m_positions = m_restPositions;
    m_pinTargets = m_restPositions;
    m_contactResponses.clear();
    m_stepInverseMasses.clear();
    std::fill(m_velocities.begin(), m_velocities.end(), Vector3{});
}

bool ClothSolver::Step(float dt, std::span<const ClothContact> contacts, std::span<const ClothMotionConstraint> motion,
                       const ClothWindSampler& wind)
{
    // 失敗したステップの接触反作用は公開しない。
    m_contactResponses.clear();
    if (m_positions.empty() || !std::isfinite(dt) || dt <= 0.0f || dt > 0.1f) {
        return false;
    }
    for (const auto& contact : contacts) {
        if (!ValidContact(contact)) {
            return false;
        }
    }

    // 今回だけ固定する質点を調べ、元の逆質量とは別に解法用の逆質量を用意する。
    m_stepInverseMasses = m_inverseMasses;
    m_motionUsed.assign(m_positions.size(), 0);
    for (const auto& constraint : motion) {
        if (constraint.particle >= m_positions.size() || !IsFiniteVector(constraint.previousCenter)
            || !IsFiniteVector(constraint.center) || !IsFiniteNonnegative(constraint.radius)) {
            return false;
        }
        const uint32_t particle = constraint.particle;
        if (m_motionUsed[particle] || (m_inverseMasses[particle] == 0.0f && constraint.radius > 0.0f)) {
            return false;
        }
        m_motionUsed[particle] = 1;
        if (constraint.radius == 0.0f) {
            m_stepInverseMasses[particle] = 0.0f;
        }
    }

    // 非有限値や計算上限に達したとき、途中まで動いた質点をステップ開始時へ戻せるようにする。
    const float substepDuration = dt / static_cast<float>(m_settings.substeps);
    if (substepDuration * substepDuration < 1.0e-16f) {
        return false;
    }
    m_stepPositions = m_positions;
    m_stepVelocities = m_velocities;
    m_contactResponses.resize(contacts.size());
    const auto rollbackStep = [this] {
        m_positions = m_stepPositions;
        m_velocities = m_stepVelocities;
        m_contactResponses.clear();
        return false;
    };
    const bool solveSurfaceContacts = m_settings.selfCollisionFaces && m_settings.selfCollisionDistance > 0.0f
        && m_settings.selfCollisionStiffness > 0.0f;
    const float damping = std::exp(-m_settings.damping * substepDuration);
    for (int substep = 0; substep < m_settings.substeps; ++substep) {
        // 固定先を補間し、動く質点は風と重力を積分して拘束を解く前の位置を予測する。
        m_previous = m_positions;
        if (!ApplyWind(substepDuration,wind)) {
            return rollbackStep();
        }
        const float substepFraction = static_cast<float>(substep + 1) / static_cast<float>(m_settings.substeps);
        for (size_t particleIndex = 0; particleIndex < m_positions.size(); ++particleIndex) {
            if (m_stepInverseMasses[particleIndex] == 0.0f) {
                m_positions[particleIndex] = Vector3::Lerp(m_stepPositions[particleIndex], m_pinTargets[particleIndex], substepFraction);
            } else {
                m_velocities[particleIndex] = (m_velocities[particleIndex] + m_settings.gravity * substepDuration) * damping;
                m_positions[particleIndex] += m_velocities[particleIndex] * substepDuration;
            }
        }

        // 乗数はこのサブステップの反復だけで累積する。
        SolveMotion(motion, substepFraction, substepDuration, false);
        for (auto& constraint : m_constraints) {
            constraint.lambda = 0.0f;
        }
        for (auto& bend : m_bends) {
            bend.lambda = 0.0f;
        }
        // 反復中の移動を見込み、予測位置の掃引を接触距離の 2 倍だけ広げて候補を固定する。
        if (solveSurfaceContacts && !BuildPrimitivePairs(2.0f * m_settings.selfCollisionDistance)) {
            return rollbackStep();
        }
        for (int iteration = 0; iteration < m_settings.iterations; ++iteration) {
            SolveDistances(substepDuration);
            SolveBending(substepDuration);
            if (!SolveSelfContacts()) {
                return rollbackStep();
            }
            if (solveSurfaceContacts) {
                SolvePrimitiveProximity();
            }
            SolveMotion(motion, substepFraction, substepDuration, false);
        }
        // CCD は反復後の最終位置で候補を取り直す。補正が別の交差を生む場合に備えて数回まで繰り返す。
        if (solveSurfaceContacts && m_settings.continuousCollision) {
            if (!BuildPrimitivePairs(m_settings.selfCollisionDistance)) {
                return rollbackStep();
            }
            for (int ccdPass = 0; ccdPass < 4 && SolvePrimitiveContinuous(); ++ccdPass) {}
        }

        // 補正後の変位から速度を求め、接触と移動制限のうち後者を最後に適用する。
        for (size_t particleIndex = 0; particleIndex < m_positions.size(); ++particleIndex) {
            m_velocities[particleIndex] = (m_positions[particleIndex] - m_previous[particleIndex]) / substepDuration;
        }
        SolveContacts(contacts, substepDuration);
        SolveMotion(motion, substepFraction, substepDuration, true);
        for (size_t particleIndex = 0; particleIndex < m_positions.size(); ++particleIndex) {
            if (!IsFiniteVector(m_positions[particleIndex]) || !IsFiniteVector(m_velocities[particleIndex])) {
                return rollbackStep();
            }
        }
    }
    for (const auto& response : m_contactResponses) {
        if (!IsFiniteVector(response.impulse) || !IsFiniteVector(response.angularImpulse)) {
            return rollbackStep();
        }
    }
    return true;
}

}
