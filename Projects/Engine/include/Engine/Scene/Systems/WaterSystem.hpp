/// @file    WaterSystem.hpp
/// @brief   水面の実効波 (.mat × 倍率 × 環境風) の解決と、剛体の着水 (波紋・しぶき) の検出。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <unordered_map>

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::scene {

struct Transform;

/// 水の .mat が持つ «種類» のキー。Engine の解決処理と Editor の Inspector が同じ名前を引く。
namespace water_keys {
inline constexpr const char* kWaveDirection[4]  = { "wave0Direction",  "wave1Direction",  "wave2Direction",  "wave3Direction"  };
inline constexpr const char* kWaveAmplitude[4]  = { "wave0Amplitude",  "wave1Amplitude",  "wave2Amplitude",  "wave3Amplitude"  };
inline constexpr const char* kWaveWavelength[4] = { "wave0Wavelength", "wave1Wavelength", "wave2Wavelength", "wave3Wavelength" };
inline constexpr const char* kWaveSteepness[4]  = { "wave0Steepness",  "wave1Steepness",  "wave2Steepness",  "wave3Steepness"  };
/// 環境風で波がどれだけ育つか [0,1]。外洋は 1、池は 0。
inline constexpr const char* kWindResponse  = "windResponse";
/// 浮いている物体を押し流す水流の速さ [m/s]。向きは kFlowDirection。
inline constexpr const char* kCurrentSpeed  = "currentSpeed";
inline constexpr const char* kFlowDirection = "flowDirection";
} // namespace water_keys

/// .mat に波のキーが無いときに使う波 (Ocean.mat と同じ値)。範囲外は振幅 0。
/// WHY 平らにしないか: 波を .mat へ移す前の .mat には波のキーが無い。そこで平らにすると、
///     更新前のプロジェクトを開いた瞬間に海が止まる。
[[nodiscard]] GerstnerWave DefaultWaterWave(int index);

/// 水面が受け取る環境風。ForceField のグローバル Wind を要約したもの。
struct WaterWind {
    bool          active    = false;
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    float         strength  = 0.0f; ///< ForceField の strength [m/s^2]
};

/// .mat の波に個体の倍率と環境風を掛けて water.waves / water.current を組み立てる。
/// 風は «追い風の波を育て、向かい風の波を潰す» だけで、波の向きは回さない。
/// WHY 向きを回さないか: 位相は k·(D·p) なので、D を回すと原点から遠い点ほど位相が跳ぶ。
///     風が変わるたびに遠景の波が早送りのように走って見える。
void ResolveWaterWaves(WaterComponent& water, const asset::MaterialAsset* material, const WaterWind& wind);

/// water.materialPath の .mat を引く。未設定・読込失敗なら nullptr。
[[nodiscard]] const asset::MaterialAsset* LoadWaterMaterial(const WaterComponent& water);

/// 頂点グリッド 1 セルのワールド実寸 [m]。extent に Transform のスケールを掛けて解像度で割る。
/// WHY 1 か所に置くか: 描画 (WaterCB) と浮力 (WaterComponent::GetSurfaceHeightAt) が
///     «刻めない波» の判断に同じ値を使う必要がある。
[[nodiscard]] math::Vector2 ResolveWaterCellSize(const WaterComponent& water, const Transform& transform);

/// ワールド座標に着水の波紋を 1 つ置く。大きさはメートルで決め、水面の広さに依らず揃える。
/// @param strength [0,1]
void EmitWaterRipple(EntityID waterEntity, const WaterComponent& water, const Transform& transform,
                     const math::Vector3& worldPos, float strength);

class WaterSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "WaterSystem"; }
    // 物理が動かした直後の位置で着水を見る。解決した波は同じフレームの描画と、次の物理ステップが読む。
    Phase            GetPhase()   const override { return Phase::PostPhysics; }
    // 編集中も走らせる。止めると .mat の波を編集している間、ビューポートの水面が動かない。
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    void Update(SystemContext& ctx) override;

private:
    struct BodyState {
        bool  submerged = false;
        float wakeTimer = 0.0f;
        bool  seen      = false;
    };
    // 剛体ごとの «前のフレームで水中に居たか»。キーは EntityID (generation << 32 | index)。
    std::unordered_map<uint64_t, BodyState> m_bodies;
};

} // namespace fbzz::scene
