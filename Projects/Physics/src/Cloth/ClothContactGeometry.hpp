/// @file    ClothContactGeometry.hpp
/// @brief   布の自己接触と相互接触で使う符号付き重みと位置射影。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once

#include <Math/LinearAlgebra/Vector3.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace fbzz::physics::cloth_detail {

using math::Vector3;

struct Vector3d {
    double x = 0.0; ///< 掃引接触の計算に使うワールド X 成分 [m]。
    double y = 0.0; ///< 掃引接触の計算に使うワールド Y 成分 [m]。
    double z = 0.0; ///< 掃引接触の計算に使うワールド Z 成分 [m]。
};
inline Vector3d ToDouble(const Vector3& v) { return {v.x, v.y, v.z}; }
inline Vector3d operator-(const Vector3d& a, const Vector3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vector3d operator+(const Vector3d& a, const Vector3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vector3d operator*(const Vector3d& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(const Vector3d& a, const Vector3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vector3d Cross(const Vector3d& a, const Vector3d& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

using ParticleQuad = std::array<uint32_t, 4>;
using ContactWeights = std::array<float, 4>;

bool BoxesOverlapYZ(const Vector3& lowerA, const Vector3& upperA,
                    const Vector3& lowerB, const Vector3& upperB);
ContactWeights PairWeights(const std::vector<Vector3>& positions, const ParticleQuad& particles,
                           bool edgeEdge, bool& outInterior);
Vector3 WeightedOffset(const std::vector<Vector3>& positions, const ParticleQuad& particles,
                       const ContactWeights& weights);
bool OrientContactNormal(const std::vector<Vector3>& positions, const std::vector<Vector3>& previous,
                         const ParticleQuad& particles, bool edgeEdge,
                         const ContactWeights& weights, Vector3& outNormal);
void ProjectSeparation(std::vector<Vector3>& positions, const std::vector<float>& inverseMasses,
                       const ParticleQuad& particles, const ContactWeights& weights,
                       const Vector3& normal, float penetration);

}
