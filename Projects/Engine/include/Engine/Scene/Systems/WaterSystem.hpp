/// @file    WaterSystem.hpp
/// @brief   水面の実効波 (.mat × 倍率 × 環境風) の解決と、剛体の着水 (波紋・しぶき) の検出。
/// @author  Hasegawa Jin
/// @date    2026-09-11
#pragma once

#include <Engine/Core/Scheduler/ISystem.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::scene {

struct Transform;

/// @note 水の .mat が持つ «種類» のキー。Engine の解決処理と Editor の Inspector が同じ名前を引く。
namespace water_keys {
inline constexpr const char* kWaveDirection[4]  = { "wave0Direction",  "wave1Direction",  "wave2Direction",  "wave3Direction"  };
inline constexpr const char* kWaveAmplitude[4]  = { "wave0Amplitude",  "wave1Amplitude",  "wave2Amplitude",  "wave3Amplitude"  };
inline constexpr const char* kWaveWavelength[4] = { "wave0Wavelength", "wave1Wavelength", "wave2Wavelength", "wave3Wavelength" };
inline constexpr const char* kWaveSteepness[4]  = { "wave0Steepness",  "wave1Steepness",  "wave2Steepness",  "wave3Steepness"  };
/// @note 環境風で波がどれだけ育つか [0,1]。外洋は 1、池は 0。
inline constexpr const char* kWindResponse  = "windResponse";
/// @note 浮いている物体を押し流す水流の速さ [m/s]。向きは kFlowDirection。
inline constexpr const char* kCurrentSpeed  = "currentSpeed";
/// @note 波の «群» の深さ [0,1]。0 でどの波頭も同じ高さ、上げるほど大きい波の塊と凪が交互に来る。
inline constexpr const char* kWaveGrouping  = "waveGrouping";
/// @note 方向広がり [0,1]。0 で «1 波 1 方向» の直線的な波頭、上げるほど波頭が有限の長さに切れる。
inline constexpr const char* kWaveSpread    = "waveSpread";
inline constexpr const char* kFlowDirection = "flowDirection";
}

/// @note .mat に波のキーが無いときに使う波 (Ocean.mat と同じ値)。範囲外は振幅 0。
/// @note 平らにしない理由: 波を .mat へ移す前のプロジェクトには波のキーが無いため、平らにすると
/// @note       更新前のプロジェクトを開いた瞬間に海が止まる。
[[nodiscard]] GerstnerWave DefaultWaterWave(int index);

/// @note 水面が受け取る環境流。SceneEnvironment の環境流を要約したもの。
struct WaterWind {
    bool          active    = false;
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    float         speed     = 0.0f; ///< @note 水面での流速 [m/s]
};

/// @note この風速で .mat が書いた振幅どおりの波になる基準 [m/s]。
/// @note これより速ければ追い風の波が育ち、遅ければ潰れる。倍率の上限 2 は
/// @note       «.mat の 2 倍まで» という見た目の歯止めで、物理的な意味は無い。
inline constexpr float kWaveGrowthReferenceWindSpeed = 10.0f;

/// @note .mat の波に個体の倍率と環境風を掛けて water.waves / water.current を組み立てる。
/// @note 風は «追い風の波を育て、向かい風の波を潰す» だけで、波の向きは回さない。
/// @note 向きを回さない理由: 位相は k·(D·p) のため D を回すと原点から遠い点ほど位相が跳ね、
/// @note       風が変わるたびに遠景の波が早送りのように走って見える。
void ResolveWaterWaves(WaterComponent& water, const asset::MaterialAsset* material, const WaterWind& wind);

/// @note water.materialPath の .mat を引く。未設定・読込失敗なら nullptr。
[[nodiscard]] const asset::MaterialAsset* LoadWaterMaterial(const WaterComponent& water);

/// @brief 物理の波と近景描画が使う最密区間の頂点間隔 [ワールド m]。
/// @note 集中格子では nearCellSize と均一格子の間隔の小さい方を使い、カメラへ依存させない。
/// @see Docs/design/water-waves.md 有限の矩形を保つカメラ集中グリッド。
[[nodiscard]] math::Vector2 ResolveWaterCellSize(const WaterComponent& water, const Transform& transform);

/// @note ワールド座標に着水の波紋を 1 つ置く。大きさはメートルで決め、水面の広さに依らず揃える。
/// @param strength [0,1]
/// @note 輪の正本は water.ripples (ワールド座標)。描画パスはそれを読んで焼くだけなので、
/// @note       ここで両方へ積む必要はない。
void EmitWaterRipple(WaterComponent& water, const Transform& transform,
                     const math::Vector3& worldPos, float strength);

/// @note 生きている輪の寿命を進め、頂点で刻めるかを測り直す。
/// @note System 側で進める理由: 描画パスでやると SceneView と GameView で 2 回進む。
void UpdateWaterRipples(WaterComponent& water, const Transform& transform, float dt);

/// @note シーンの流れから «この水面の形と質感に出るもの» を選び、water.surfaceFlows へ書く。
///
/// @note 拾うのは半径 (Baked は extents) を持ち、XZ がその半径ぶん広げた矩形と交わるもので、
/// @note (|形の高さ|, 流速) の強い順に kWaterSurfaceFlowCount 本まで。
/// @note 半径の無い要素と横倒しの渦を外すのは «どこまで効くか» が決まらないため。流速としては
/// @note       SampleFlow が出し続けるので、浮力と水中判定からは消えない。
/// @warning Baked の vectorField ポインタは今フレームだけ有効。毎フレーム呼び直すこと。
/// @see Docs/design/water-waves.md 「流れの場が水面に出る 3 つの道」
void ResolveWaterSurfaceFlows(WaterComponent& water, const Transform& transform,
                              const std::vector<ActiveFlowField>& fields);

/// @note 水面の «その点の» 流速 [m/s] (ワールド、y は常に 0)。
///
/// @param surfaceBaseY 水面 GameObject のワールド Y。流れは水面の高さでサンプルする。
/// @note .mat の一様な current に、その点を覆う流れの場を足したもの。浮いた物が渦の周りを
/// @note       回るのはこれが場所の関数になっているため。
[[nodiscard]] math::Vector3 WaterFlowVelocityAt(const WaterComponent& water,
                                                const std::vector<ActiveFlowField>& fields,
                                                float surfaceBaseY,
                                                const math::Vector3& worldPos,
                                                float time);

class WaterSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "WaterSystem"; }
    /// @note 物理が動かした直後の位置で着水を見る。解決した波は同じフレームの描画と、次の物理ステップが読む。
    Phase            GetPhase()   const override { return Phase::PostPhysics; }
    /// @note 編集中も走らせる。止めると .mat の波を編集している間、ビューポートの水面が動かない。
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    void Update(SystemContext& ctx) override;

private:
    struct BodyState {
        bool  submerged = false;
        float wakeTimer = 0.0f;
        bool  seen      = false;
    };
    /// @note 剛体ごとの «前のフレームで水中に居たか»。キーは EntityID (generation << 32 | index)。
    std::unordered_map<uint64_t, BodyState> m_bodies;
};

}
