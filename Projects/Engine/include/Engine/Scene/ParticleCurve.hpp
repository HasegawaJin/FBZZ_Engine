/// @file    ParticleCurve.hpp
/// @brief   正規化時間に対する値 / 色の推移を持つ軽量カーブとグラデーション。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// @note ParticleEmitter.hpp から分離: VFX フェードやダメージ減衰にも使う汎用型で、
///       ParticleEmitter.hpp は Script.hpp を include するため循環を避けた。
/// @note 依存を持たないため、スクリプトは Script.hpp だけでカーブフィールドを宣言できる。

#pragma once
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

/// @brief 正規化時間に対する値 1 点。
/// @note 固定長にして GPU 定数バッファへそのまま転送できるようにする。
struct ParticleCurveKey {
    float time = 0.0f;
    float value = 0.0f;
    bool operator==(const ParticleCurveKey&) const = default;
};

/// @brief カーブ / グラデーションのキー上限。
/// @note 4 キーでは立ち上がり→保持→減衰→余韻のような4区間を表せず、8 キーで実用上足りる。
/// @note GPU 定数バッファは float4 が 1 キー 2 点、curve 1 本で 4 レジスタ。上限を上げる場合は HLSL 側も対で直すこと。
inline constexpr uint32_t kMaxParticleCurveKeys = 8;

/// @brief キー間の繋ぎ方。キー単位ではなくカーブ単位に持つ。
/// @note キー単位にすると GPU 側で 1 キーごとに float が増えパッキングが崩れる。混在が要る場合はキーを増やして近似する。
enum class ParticleCurveInterpolation : uint8_t {
    Linear = 0, ///< 直線
    Step,       ///< 次のキーまで前の値を保持 (フリップブックの段階切替・点滅)
    Smooth,     ///< smoothstep。始点と終点で速度 0 になり、機械的な折れ線に見えない
};

/// @brief 補間係数へ曲線モードを適用する。
/// @note CPU/GPU で必ず同じ式にすること (GPU 側は ParticleGpuSim.cs.hlsl の ApplyCurveInterpolation)。
inline float ApplyCurveInterpolation(float alpha, ParticleCurveInterpolation mode)
{
    /// @note Step は次のキーへ到達した時点で切り替える (区間は [前のキー, 次のキー))。
    /// @note 評価は「次のキー以下」の区間で行うため、常に 0 だと寿命 0..1 の t=1.0 (最終フレーム) で
    ///       フリップブックの最終コマや消え際の点滅が一度も表示されないまま粒子が死ぬ。
    if (mode == ParticleCurveInterpolation::Step) return alpha >= 1.0f ? 1.0f : 0.0f;
    if (mode == ParticleCurveInterpolation::Smooth) return alpha * alpha * (3.0f - 2.0f * alpha);
    return alpha;
}

/// @brief 軽量カーブ。Editor で最大 kMaxParticleCurveKeys キーを編集する。
struct ParticleCurve {
    std::array<ParticleCurveKey, kMaxParticleCurveKeys> keys{{
        {0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f},
        {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}
    }};
    uint32_t keyCount = 2;
    ParticleCurveInterpolation interpolation = ParticleCurveInterpolation::Linear;

    float Evaluate(float time) const
    {
        const uint32_t count = keyCount < 1 ? 1 : (keyCount > keys.size() ? static_cast<uint32_t>(keys.size()) : keyCount);
        if (time <= keys[0].time) return keys[0].value;
        for (uint32_t index = 1; index < count; ++index) {
            if (time <= keys[index].time) {
                const float span = (std::max)(keys[index].time - keys[index - 1].time, 0.0001f);
                float alpha = (std::max)(0.0f, (std::min)(1.0f, (time - keys[index - 1].time) / span));
                alpha = ApplyCurveInterpolation(alpha, interpolation);
                return keys[index - 1].value + (keys[index].value - keys[index - 1].value) * alpha;
            }
        }
        return keys[count - 1].value;
    }
};

struct ParticleGradientKey {
    float time = 0.0f;
    math::Vector4 color = { 1, 1, 1, 1 };
    bool operator==(const ParticleGradientKey& other) const
    {
        return time == other.time
            && color.x == other.color.x && color.y == other.color.y
            && color.z == other.color.z && color.w == other.color.w;
    }
};

/// @brief GPU 転送可能な色 Gradient。キー上限はカーブと共通。Step 補間は炎→煙のような硬い切り替えに使う。
/// @note キーの RGB はカラーピッカー表示値 (sRGB) でオーサリングし、1 を超えてよい (HDR)。シェーダー直前で一度だけリニア変換する。
/// @note CPU (EvaluateLinear) と GPU (ParticleGpuSim.cs.hlsl の EvaluateGradient8) は同じ順序 (補間→リニア化) で処理すること。
struct ParticleGradient {
    std::array<ParticleGradientKey, kMaxParticleCurveKeys> keys{{
        {0.0f, {1, 1, 1, 1}}, {1.0f, {1, 1, 1, 0}},
        {1.0f, {1, 1, 1, 0}}, {1.0f, {1, 1, 1, 0}},
        {1.0f, {1, 1, 1, 0}}, {1.0f, {1, 1, 1, 0}},
        {1.0f, {1, 1, 1, 0}}, {1.0f, {1, 1, 1, 0}}
    }};
    uint32_t keyCount = 2;
    ParticleCurveInterpolation interpolation = ParticleCurveInterpolation::Linear;
    ParticleColorSpace colorSpace = ParticleColorSpace::Gamma;

    /// @brief オーサリング空間 (sRGB) で評価する。
    /// @note Editor のプレビュー帯やカラーピッカーはこちらを使う (ImGui は sRGB 値を受け取る前提)。
    math::Vector4 Evaluate(float time) const
    {
        const uint32_t count = keyCount < 1 ? 1 : (keyCount > keys.size() ? static_cast<uint32_t>(keys.size()) : keyCount);
        if (time <= keys[0].time) return keys[0].color;
        for (uint32_t index = 1; index < count; ++index) {
            if (time <= keys[index].time) {
                const float span = (std::max)(keys[index].time - keys[index - 1].time, 0.0001f);
                float alpha = (std::max)(0.0f, (std::min)(1.0f, (time - keys[index - 1].time) / span));
                alpha = ApplyCurveInterpolation(alpha, interpolation);
                return MixKeys(keys[index - 1].color, keys[index].color, alpha);
            }
        }
        return keys[count - 1].color;
    }

    /// @brief 描画へ渡すリニア色。シミュレーションが粒子へ書くのは常にこちら。
    math::Vector4 EvaluateLinear(float time) const
    {
        return ParticleSrgbToLinear(Evaluate(time));
    }

    /// @brief 2 キーを colorSpace に従って混ぜる。
    /// @note アルファは常に線形で混ぜる (不透明度は光量ではないため)。
    math::Vector4 MixKeys(const math::Vector4& a, const math::Vector4& b, float alpha) const
    {
        const float w = a.w + (b.w - a.w) * alpha;
        switch (colorSpace) {
        case ParticleColorSpace::Linear: {
            const math::Vector4 la = ParticleSrgbToLinear(a);
            const math::Vector4 lb = ParticleSrgbToLinear(b);
            const math::Vector4 mixed = { la.x + (lb.x - la.x) * alpha,
                                          la.y + (lb.y - la.y) * alpha,
                                          la.z + (lb.z - la.z) * alpha, w };
            return ParticleLinearToSrgb(mixed);
        }
        case ParticleColorSpace::Oklab: {
            const math::Vector4 la = ParticleSrgbToLinear(a);
            const math::Vector4 lb = ParticleSrgbToLinear(b);
            const math::Vector3 oa = ParticleLinearToOklab({ la.x, la.y, la.z });
            const math::Vector3 ob = ParticleLinearToOklab({ lb.x, lb.y, lb.z });
            const math::Vector3 om = { oa.x + (ob.x - oa.x) * alpha,
                                       oa.y + (ob.y - oa.y) * alpha,
                                       oa.z + (ob.z - oa.z) * alpha };
            const math::Vector3 back = ParticleOklabToLinear(om);
            return ParticleLinearToSrgb({ back.x, back.y, back.z, w });
        }
        default:
            return { a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha,
                     a.z + (b.z - a.z) * alpha, w };
        }
    }
};

} // namespace fbzz::scene
