/// @file    VolumeComponent.hpp
/// @brief   トリガー領域に付与する物理効果コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-22
///
/// @note 重力・渦・爆風などの VolumeType と効果パラメーターを保持する。
/// @note 具体的な力の適用は physics / system 側で処理する。
#pragma once
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Physics/World/BodyHandle.hpp>
#include <Physics/Volumes/ColliderVolume.hpp>

namespace fbzz::scene {

struct VolumeComponent {
    physics::VolumeHandle volumeHandle; ///< 物理ワールドに登録した、この効果領域のハンドル。
    physics::VolumeType type = physics::VolumeType::Gravity; ///< 領域内の剛体へ適用する重力、渦、磁場、爆風、時間倍率の種類。
    bool enabled = true; ///< この効果領域を物理ワールドの更新対象とする。

    math::Vector3 gravity = { 0.0f, -9.81f, 0.0f }; ///< Gravity 領域の剛体に適用するワールド重力加速度（m/s²）。
    math::Vector3 magneticField = { 0.0f, 1.0f, 0.0f }; ///< 剛体の電荷と速度から磁気力を求めるワールド磁場ベクトル。

    float swirlStrength = 1.0f; ///< Vortex の中心を回る XZ 接線方向の加速度（m/s²）。
    float inwardStrength = 0.0f; ///< Vortex の XZ 中心へ向かう加速度（m/s²）。負値で外向きに作用する。
    float liftStrength = 0.0f; ///< Vortex 領域内の剛体に加える上向き加速度（m/s²）。
    float explosionImpulse = 10.0f; ///< Explosion の中心から外向きに作用する瞬間力積の大きさ（N·s）。
    float timeScale = 1.0f; ///< TimeDilation の内部で剛体の更新時間に掛ける倍率。
    float duration  = -1.0f; ///< 負値は無限継続。正値は秒単位の有効期間
    float elapsed   = 0.0f; ///< 経過時間。duration >= 0 のときだけ PhysicsSystem が加算する

    const char* GetTypeName() const { return "Volume"; }
    void Reflect(IReflector& r)
    {
        // VolumeType を int 経由で反映する (LightComponent と同じパターン)。
        // 保存されるのは整数そのものなので、列挙から値を消しても詰められない。
        // 2 は廃止した Buoyancy の枠。浮力は WaterComponent が持つので、読み込み側
        // (SceneSerializer) がこの Volume を無効化して 1 行知らせる。
        /// @see Docs/design/buoyancy.md
        int typeValue = static_cast<int>(type);
        r.Field("type", typeValue);
        if (typeValue < 0 || typeValue > 5 || typeValue == 2) {
            typeValue = 0;
        }
        type = static_cast<physics::VolumeType>(typeValue);

        r.Field("enabled", enabled);
        r.Field("gravity", gravity);
        r.Field("magneticField", magneticField);
        r.Field("swirlStrength", swirlStrength);
        r.Field("inwardStrength", inwardStrength);
        r.Field("liftStrength", liftStrength);
        // 旧キー drag は書かない。どの VolumeType も読んでいなかった死んだノブで、
        // 流れの抵抗は physics::FlowVolume と剛体側の flowCoupling が持つ。
        // 旧シーンの drag は «Reflect が尋ねないキー» として黙って落ちる。
        /// @see Docs/design/flow-field.md
        r.Field("explosionImpulse", explosionImpulse);
        r.Field("timeScale", timeScale);
        r.Field("duration", duration);
        r.Field("elapsed", elapsed);
    }
};

}
