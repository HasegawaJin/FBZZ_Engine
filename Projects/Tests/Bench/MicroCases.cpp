/// @file    MicroCases.cpp
/// @brief   Math の micro 計測の入力生成と各演算。
/// @author  Hasegawa Jin
/// @date    2026-09-18
#include "MicroCases.hpp"

#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <array>
#include <cstdint>

namespace fbzz::bench {

namespace {

using math::Frustum;
using math::Matrix4;
using math::Quaternion;
using math::Vector3;

/// @brief 入力の要素数。L1 に収まる大きさにして、メモリ帯域ではなく演算そのものを測る。
constexpr int kInputCount = 1024;
constexpr int kInputMask  = kInputCount - 1;

/// @brief 決定的な擬似乱数 (線形合同法)。実行ごとに入力が変わると前後比較が成り立たない。
/// @see https://en.wikipedia.org/wiki/Linear_congruential_generator Numerical Recipes の定数 (a=1664525, c=1013904223)
class Lcg {
public:
    /// @return [-1, 1) の一様な値。
    float Next()
    {
        m_state = m_state * 1664525u + 1013904223u;
        return static_cast<float>(m_state >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }

private:
    uint32_t m_state = 0x12345678u;
};

struct Inputs {
    std::array<Vector3, kInputCount>    vectors;
    std::array<Vector3, kInputCount>    extents;
    std::array<Quaternion, kInputCount> rotations;
    std::array<Matrix4, kInputCount>    matrices;
    Frustum                             frustum;
};

const Inputs& GetInputs()
{
    static const Inputs inputs = [] {
        Inputs result;
        Lcg random;
        for (int i = 0; i < kInputCount; ++i) {
            result.vectors[i] = { random.Next() * 50.0f, random.Next() * 50.0f, random.Next() * 50.0f + 50.0f };
            result.extents[i] = { random.Next() + 1.5f, random.Next() + 1.5f, random.Next() + 1.5f };
            const Vector3 axis = { random.Next(), random.Next() + 2.0f, random.Next() };
            result.rotations[i] = Quaternion::FromAxisAngle(axis, random.Next() * math::PI);
            result.matrices[i] = Matrix4::TRS(result.vectors[i], result.rotations[i], result.extents[i]);
        }
        const Matrix4 view = Matrix4::LookAt({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, Vector3::UP);
        const Matrix4 projection = Matrix4::Perspective(60.0f * math::DEG2RAD, 16.0f / 9.0f, 0.1f, 200.0f);
        result.frustum = Frustum::FromViewProjection(projection * view);
        return result;
    }();
    return inputs;
}

/// @brief 演算結果の書き込み先。結果は全成分をここへ書く。
/// @note 一部の成分だけを足し込むと、インライン化された関数では残りの成分の計算が最適化で消え、実際の使い方 (cbuffer・骨の行列の配列へ書く) より軽く測れてしまう。
struct Outputs {
    std::array<Matrix4, kInputCount>       matrices;
    std::array<math::Vector4, kInputCount> vectors4;
    std::array<Vector3, kInputCount>       vectors3;
};

Outputs& GetOutputs()
{
    static Outputs outputs;
    return outputs;
}

/// @return 実行時に決まる位置の成分。どの書き込みも読まれうるので、最適化で書き込みを省けない。
double Pick(const Matrix4& m)        { return m.m[1][2]; }
double Pick(const math::Vector4& v)  { return v.y; }
double Pick(const Vector3& v)        { return v.y; }

double Matrix4Multiply(int ops)
{
    const Inputs& in = GetInputs();
    auto& out = GetOutputs().matrices;
    for (int i = 0; i < ops; ++i)
        out[i & kInputMask] = in.matrices[i & kInputMask] * in.matrices[(i + 1) & kInputMask];
    return Pick(out[ops & kInputMask]);
}

double Matrix4Vector4(int ops)
{
    const Inputs& in = GetInputs();
    auto& out = GetOutputs().vectors4;
    for (int i = 0; i < ops; ++i) {
        const Vector3& p = in.vectors[(i + 3) & kInputMask];
        out[i & kInputMask] = in.matrices[i & kInputMask] * math::Vector4{ p, 1.0f };
    }
    return Pick(out[ops & kInputMask]);
}

/// @note cbuffer へ送る前に毎回呼ぶ (HLSL は列優先で読む)。
double Matrix4Transpose(int ops)
{
    const Inputs& in = GetInputs();
    auto& out = GetOutputs().matrices;
    for (int i = 0; i < ops; ++i)
        out[i & kInputMask] = Matrix4::Transpose(in.matrices[i & kInputMask]);
    return Pick(out[ops & kInputMask]);
}

double Matrix4Trs(int ops)
{
    const Inputs& in = GetInputs();
    auto& out = GetOutputs().matrices;
    for (int i = 0; i < ops; ++i) {
        const int k = i & kInputMask;
        out[k] = Matrix4::TRS(in.vectors[k], in.rotations[k], in.extents[k]);
    }
    return Pick(out[ops & kInputMask]);
}

double Matrix4Inverse(int ops)
{
    const Inputs& in = GetInputs();
    auto& out = GetOutputs().matrices;
    for (int i = 0; i < ops; ++i)
        out[i & kInputMask] = Matrix4::Inverse(in.matrices[i & kInputMask]);
    return Pick(out[ops & kInputMask]);
}

double QuaternionRotate(int ops)
{
    const Inputs& in = GetInputs();
    auto& out = GetOutputs().vectors3;
    for (int i = 0; i < ops; ++i)
        out[i & kInputMask] = in.rotations[i & kInputMask] * in.vectors[(i + 7) & kInputMask];
    return Pick(out[ops & kInputMask]);
}

double Vector3Normalize(int ops)
{
    const Inputs& in = GetInputs();
    auto& out = GetOutputs().vectors3;
    for (int i = 0; i < ops; ++i)
        out[i & kInputMask] = in.vectors[i & kInputMask].Normalized();
    return Pick(out[ops & kInputMask]);
}

double FrustumAabb(int ops)
{
    const Inputs& in = GetInputs();
    int inside = 0;
    for (int i = 0; i < ops; ++i) {
        const int k = i & kInputMask;
        inside += in.frustum.IntersectsAABB(in.vectors[k], in.extents[k]) ? 1 : 0;
    }
    return inside;
}

/// @note 描画のカリング (GeometryPassHelpers / ShadowPass) が物体ごとに呼ぶ形。半径は extents.x を流用する。
double FrustumSphere(int ops)
{
    const Inputs& in = GetInputs();
    int inside = 0;
    for (int i = 0; i < ops; ++i) {
        const int k = i & kInputMask;
        inside += in.frustum.IntersectsSphere(in.vectors[k], in.extents[k].x) ? 1 : 0;
    }
    return inside;
}

} // namespace

const std::vector<MicroCase>& AllMicroCases()
{
    static const std::vector<MicroCase> cases = {
        { "Matrix4 * Matrix4",        &Matrix4Multiply },
        { "Matrix4 * Vector4",        &Matrix4Vector4 },
        { "Matrix4::Transpose",       &Matrix4Transpose },
        { "Matrix4::TRS",             &Matrix4Trs },
        { "Matrix4::Inverse",         &Matrix4Inverse },
        { "Quaternion * Vector3",     &QuaternionRotate },
        { "Vector3::Normalized",      &Vector3Normalize },
        { "Frustum::IntersectsAABB",  &FrustumAabb },
        { "Frustum::IntersectsSphere", &FrustumSphere },
    };
    return cases;
}

} // namespace fbzz::bench
