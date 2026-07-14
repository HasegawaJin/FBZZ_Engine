// FBZZ Engine
// ParticleForceField.hpp | fbzz::scene
// パーティクル力場コンポーネント (ベクトルフィールド)
// シーン内の ParticleEmitter 粒子へ風・吸引・渦・乱流などの力を加える。
// ParticlePass が毎フレーム全力場を収集し、CPU/GPU 両方のシミュレーションで同じ式を適用する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

// 力場の種類。
// WHY: エミッター単体の gravity/velocityDamping では表現できない「空間の場」による挙動を
//      種類ごとに分ける。数値は GPU 定数バッファへ float として渡すため順序を変更しないこと。
enum class ParticleForceFieldType : uint8_t {
    Wind = 0,    // direction 方向へ一定加速 (風)
    Attract,     // 力場中心へ引き寄せる (吸引)
    Repulse,     // 力場中心から遠ざける (反発)
    Vortex,      // direction を軸とした接線方向の渦
    Turbulence,  // カールノイズによる発散ゼロの乱流ベクトル場
    Drag,        // 速度に比例した減速 (空気抵抗)
};

// ParticleForceField — パーティクルへ作用する力場 1 つ分の設定。
// 力場の中心は GameObject の Transform.worldPosition、
// direction は Transform.worldRotation で回転してワールド空間へ変換される。
// WHY: エミッターから独立した GameObject として配置することで、
//      1 つの風・渦を複数エミッターへ同時に効かせたり、動く力場を作れるようにする。
struct ParticleForceField {
    bool enabled = true;
    ParticleForceFieldType fieldType = ParticleForceFieldType::Wind;
    // 加速度の大きさ [m/s^2]。Drag のときは減衰係数 [1/s] として扱う。
    float strength = 5.0f;
    // 影響半径 [m]。0 以下でシーン全体へ減衰なしに作用する (グローバル風など)。
    float radius = 5.0f;
    // 距離減衰カーブ: influence = (1 - dist/radius)^falloffPower。radius <= 0 のとき無効。
    float falloffPower = 2.0f;
    // Wind: 風向き / Vortex: 回転軸 (ローカル空間)。
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    // Turbulence 用: ノイズ格子の空間周波数 [1/m] とスクロール速度。
    float noiseFrequency = 0.5f;
    float noiseSpeed = 1.0f;

    const char* GetTypeName() const { return "Particle Force Field"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int typeValue = static_cast<int>(fieldType);
        r.Field("fieldType", typeValue);
        typeValue = typeValue < 0 ? 0 : (typeValue > 5 ? 5 : typeValue);
        fieldType = static_cast<ParticleForceFieldType>(typeValue);
        r.Field("strength", strength);
        r.Field("radius", radius);
        r.Field("falloffPower", falloffPower);
        r.Field("direction", direction);
        r.Field("noiseFrequency", noiseFrequency);
        r.Field("noiseSpeed", noiseSpeed);
    }
};

} // namespace fbzz::scene
