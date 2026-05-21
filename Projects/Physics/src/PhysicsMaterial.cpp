// FBZZ Engine
// PhysicsMaterial.cpp | fbzz::physics
// 剛体の表面物性 (反発・摩擦・密度) のプリセット定義
#include <Physics/PhysicsMaterial.hpp>
#include <algorithm> // std::min, std::sqrt
#include <cmath>   // std::sqrt

namespace fbzz::physics
{
    float PhysicsMaterial::CombineRestitution(const PhysicsMaterial& a, const PhysicsMaterial& b) {
        return std::min(a.restitution, b.restitution);
    }

    // 摩擦: 幾何平均 (両者の中間)
    float PhysicsMaterial::CombineFriction(const PhysicsMaterial& a, const PhysicsMaterial& b) {
        return std::sqrt(a.dynamicFriction * b.dynamicFriction);
    }

    const PhysicsMaterial PhysicsMaterial::Default = { 0.3f, 0.6f, 0.4f, 1.0f };
    const PhysicsMaterial PhysicsMaterial::Rubber  = { 0.8f, 1.0f, 0.9f, 1.2f };
    const PhysicsMaterial PhysicsMaterial::Ice     = { 0.05f, 0.05f, 0.02f, 0.9f };
    const PhysicsMaterial PhysicsMaterial::Metal   = { 0.4f, 0.4f, 0.3f, 7.8f };
    const PhysicsMaterial PhysicsMaterial::Wood    = { 0.2f, 0.7f, 0.6f, 0.6f };
    const PhysicsMaterial PhysicsMaterial::Stone   = { 0.1f, 0.9f, 0.8f, 2.5f };
} // namespace fbzz::physics