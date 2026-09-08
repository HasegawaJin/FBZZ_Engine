/// @file    ParticleCurve.hpp
/// @brief   正規化時間に対する値 / 色の推移を持つ軽量カーブとグラデーション。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// WHY ParticleEmitter.hpp から切り出すか:
///   この 2 型はパーティクル専用ではなく、VFX ノードのフェードやスクリプトの
///   ダメージ減衰にも使う汎用のオーサリング型である。ParticleEmitter.hpp は
///   Script.hpp を include するため、Script.hpp から参照すると include が循環する。
///   依存を持たないここへ置くことで、ユーザースクリプトが Script.hpp 1 枚のまま
///   カーブフィールドを宣言できる。

#pragma once
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

// ParticleCurveKey — 正規化時間に対する値 1 点。
// 固定長にしてGPU定数バッファへそのまま転送できるようにする。
struct ParticleCurveKey {
    float time = 0.0f;
    float value = 0.0f;
    bool operator==(const ParticleCurveKey&) const = default;
};

// カーブ / グラデーションのキー上限。
// WHY: 4 キーでは「立ち上がり → 保持 → 減衰 → 余韻」のような 4 区間すら表せず、
//      爆発の閃光やループする炎の呼吸を作るのに足りなかった。8 キーあれば
//      実用上の作り込みは足りる。GPU 定数バッファは float4 が 1 キー 2 点なので
//      curve 1 本あたり 4 レジスタで収まる (上限を上げる場合は HLSL 側も対で直すこと)。
inline constexpr uint32_t kMaxParticleCurveKeys = 8;

// キー間の繋ぎ方。キー単位ではなくカーブ単位に持つ。
// WHY: キー単位にすると GPU へ 1 キーあたり追加の float が要り、パッキングが崩れる。
//      実用上「このカーブ全体をなめらかにしたい / 階段にしたい」が大半で、
//      混在が要る場面はキーを増やして近似できる。
enum class ParticleCurveInterpolation : uint8_t {
    Linear = 0, // 直線
    Step,       // 次のキーまで前の値を保持 (フリップブックの段階切替・点滅)
    Smooth,     // smoothstep。始点と終点で速度 0 になり、機械的な折れ線に見えない
};

// 補間係数へ曲線モードを適用する。CPU/GPU で必ず同じ式にすること
// (GPU 側は ParticleGpuSim.cs.hlsl の ApplyCurveInterpolation)。
inline float ApplyCurveInterpolation(float alpha, ParticleCurveInterpolation mode)
{
    // Step は次のキーへ «到達した時点で» 切り替える。区間は [前のキー, 次のキー)。
    //
    // WHY 常に 0 にしないか: 評価は «次のキー以下» の区間で行われるので、常に 0 だと
    //     最後のキーの値はその時刻を越えたときにしか出ない。寿命 0..1 のカーブでは
    //     t=1.0 が最後のフレームなので、フリップブックの最終コマや消え際の点滅が
    //     一度も表示されないまま粒子が死ぬ。
    if (mode == ParticleCurveInterpolation::Step) return alpha >= 1.0f ? 1.0f : 0.0f;
    if (mode == ParticleCurveInterpolation::Smooth) return alpha * alpha * (3.0f - 2.0f * alpha);
    return alpha;
}

// ParticleCurve — 軽量カーブ。Editorで最大 kMaxParticleCurveKeys キーを編集する。
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

// ParticleGradient — GPU転送可能な色Gradient。キー上限はカーブと共通。
// Step 補間は「炎から煙へ切り替わる瞬間」のような硬い変化を作るのに使う。
//
// 色空間の規約 (エンジン全体で 1 つ):
//   キーの RGB は「カラーピッカーに表示される値」= sRGB でオーサリングする。
//   RGB は 1 を超えてよい (HDR)。シェーダーへ渡る直前に一度だけリニアへ変換する。
//   CPU 経路は EvaluateLinear、GPU 経路は ParticleGpuSim.cs.hlsl の EvaluateGradient8 が
//   同じ順序 (補間 → リニア化) で処理する。片方だけ変えると CPU/GPU で色が食い違う。
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

    // オーサリング空間 (sRGB) で評価する。Editor のプレビュー帯やカラーピッカーは
    // こちらを使う (ImGui は sRGB 値を受け取る前提のため)。
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

    // 描画へ渡すリニア色。シミュレーションが粒子へ書くのは常にこちら。
    math::Vector4 EvaluateLinear(float time) const
    {
        return ParticleSrgbToLinear(Evaluate(time));
    }

    // 2 キーを colorSpace に従って混ぜる。アルファは常に線形 (不透明度は光量ではないため)。
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
