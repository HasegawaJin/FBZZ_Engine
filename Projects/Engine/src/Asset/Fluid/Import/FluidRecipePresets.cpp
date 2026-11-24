/// @file    FluidRecipePresets.cpp
/// @brief   流体レシピのプリセットごとの部品とベイク設定。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include <Engine/Asset/Fluid/Import/FluidRecipeCodec.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace fbzz::asset {
namespace {

using namespace fbzz::fluid;

class FluidPresetBuilder {
public:
    static FluidRecipe Build(FluidPreset preset);

private:
    FluidPresetBuilder() = default;

    void ConfigurePreset(FluidPreset preset);
    void ConfigureBakeDefaults();
    void ConfigureLiquidDefaults();

    void ConfigureSmoke();
    void ConfigureFire();
    void ConfigureExplosion();
    void ConfigureSteam();
    void ConfigureDustBurst();
    void ConfigureInk();
    void ConfigureMagicWisp();
    void ConfigureHeatHaze();
    void ConfigureWaterSplash();
    void ConfigureWaterJet();
    void ConfigureBloodBurst();
    void ConfigureLavaBlob();
    void ConfigurePlasmaBurst();
    void ConfigureArcHaze();
    void ConfigureGroundRing();
    void ConfigureColdMist();
    void ConfigureChargeVortex();
    void ConfigureEmberBurst();
    void ConfigureSigilFlare();

    /// @note 返した参照は、発生源の配列を拡張すると無効になりうる。
    FluidSource& AddSource(const char* name, FluidSourceShape shape, const math::Vector3& center,
                           const math::Vector3& size);
    FluidSource& AddSphericalSource(const char* name, const math::Vector3& center, float radius);
    FluidSource& AddRingSource(const char* name, const math::Vector3& center, float radius, float tubeRadius);
    FluidSource& AddConeSource(const char* name, const math::Vector3& center, const math::Vector3& direction,
                               float radius, float length);
    /// @note 返した参照は、力の配列を拡張すると無効になりうる。
    FluidForce& AddForce(const char* name, FluidForceType type, float strength);

    FluidRecipe m_recipe;
};

FluidRecipe FluidPresetBuilder::Build(FluidPreset preset)
{
    FluidPresetBuilder builder;
    builder.m_recipe.seed = 1u + static_cast<uint32_t>(preset);
    builder.ConfigurePreset(preset);
    builder.ConfigureBakeDefaults();
    return std::move(builder.m_recipe);
}

void FluidPresetBuilder::ConfigurePreset(FluidPreset preset)
{
    switch (preset) {
    case FluidPreset::Smoke:
        ConfigureSmoke();
        break;
    case FluidPreset::Fire:
        ConfigureFire();
        break;
    case FluidPreset::Explosion:
        ConfigureExplosion();
        break;
    case FluidPreset::Steam:
        ConfigureSteam();
        break;
    case FluidPreset::DustBurst:
        ConfigureDustBurst();
        break;
    case FluidPreset::Ink:
        ConfigureInk();
        break;
    case FluidPreset::MagicWisp:
        ConfigureMagicWisp();
        break;
    case FluidPreset::HeatHaze:
        ConfigureHeatHaze();
        break;
    case FluidPreset::WaterSplash:
        ConfigureWaterSplash();
        break;
    case FluidPreset::WaterJet:
        ConfigureWaterJet();
        break;
    case FluidPreset::BloodBurst:
        ConfigureBloodBurst();
        break;
    case FluidPreset::LavaBlob:
        ConfigureLavaBlob();
        break;
    case FluidPreset::PlasmaBurst:
        ConfigurePlasmaBurst();
        break;
    case FluidPreset::ArcHaze:
        ConfigureArcHaze();
        break;
    case FluidPreset::GroundRing:
        ConfigureGroundRing();
        break;
    case FluidPreset::ColdMist:
        ConfigureColdMist();
        break;
    case FluidPreset::ChargeVortex:
        ConfigureChargeVortex();
        break;
    case FluidPreset::EmberBurst:
        ConfigureEmberBurst();
        break;
    case FluidPreset::SigilFlare:
        ConfigureSigilFlare();
        break;
    case FluidPreset::Count:
    default:
        AddSphericalSource("Source", { 0.0f, -0.6f, 0.0f }, 0.18f);
        break;
    }
}

void FluidPresetBuilder::ConfigureBakeDefaults()
{
    FluidBakeSettings& bakeSettings = m_recipe.bake;
    bakeSettings.mode = FluidBakeMode::Volume3D;
    /// @note 超解像で煙の縁のちらつきと液面のギザギザを抑える。設定は 2D / 3D で共通。
    m_recipe.output.supersampling = 2;
    if (m_recipe.kind == FluidKind::Liquid) {
        /// @note 粒子の太さに対して 64³ で液面が足りる。粒子のプリセットは確認済みの CPU ソルバーを使う。
        bakeSettings.volumeResolution = 64;
        bakeSettings.solver = FluidBakeSolver::Cpu;
        return;
    }

    bakeSettings.volumeResolution = 128;
    /// @note 発光だけの Glow や歪みマップは 6 方向ライトマップを使わない。
    const FluidShading shading = m_recipe.render.shading;
    bakeSettings.sixWayLightmaps = shading == FluidShading::Smoke || shading == FluidShading::Fire;
}

void FluidPresetBuilder::ConfigureLiquidDefaults()
{
    m_recipe.kind = FluidKind::Liquid;
    m_recipe.render.shading = FluidShading::Liquid;
}

FluidSource& FluidPresetBuilder::AddSource(const char* name, FluidSourceShape shape, const math::Vector3& center,
                                          const math::Vector3& size)
{
    FluidSource& source = m_recipe.sources.emplace_back();
    source.name = name;
    source.shape = shape;
    source.center = center;
    source.size = size;
    return source;
}

FluidSource& FluidPresetBuilder::AddSphericalSource(const char* name, const math::Vector3& center, float radius)
{
    return AddSource(name, FluidSourceShape::Sphere, center, { radius, radius, radius });
}

FluidSource& FluidPresetBuilder::AddRingSource(const char* name, const math::Vector3& center, float radius,
                                              float tubeRadius)
{
    FluidSource& source = AddSource(name, FluidSourceShape::Ring, center, { radius, tubeRadius, tubeRadius });
    source.direction = { 0.0f, 1.0f, 0.0f };
    return source;
}

FluidSource& FluidPresetBuilder::AddConeSource(const char* name, const math::Vector3& center,
                                              const math::Vector3& direction, float radius, float length)
{
    FluidSource& source = AddSource(name, FluidSourceShape::Cone, center, { radius, length, radius });
    source.direction = direction;
    return source;
}

FluidForce& FluidPresetBuilder::AddForce(const char* name, FluidForceType type, float strength)
{
    FluidForce& force = m_recipe.forces.emplace_back();
    force.name = name;
    force.type = type;
    force.strength = strength;
    return force;
}

void FluidPresetBuilder::ConfigureSmoke()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 芯・外周リング・左右の耳の 3 役で湧かせ、repulse で押し開いてタイルを埋めるまで育てる。
    /// @note 1 つの球だけだと «小さな煙玉が中央でくすぶる» 絵にしかならない。
    FluidSource& core = AddSphericalSource("Core", { 0.0f, -0.5f, 0.0f }, 0.26f);
    core.density = 6.0f;
    core.temperature = 3.0f;
    core.velocity = { 0.0f, 0.8f, 0.0f };
    core.duration = 0.5f;
    core.noise = 0.6f;
    FluidSource& collar = AddRingSource("Collar", { 0.0f, -0.48f, 0.0f }, 0.32f, 0.11f);
    collar.density = 3.5f;
    collar.temperature = 1.2f;
    collar.duration = 0.45f;
    collar.noise = 0.8f;
    collar.colorKey = 1.0f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& wing = AddSphericalSource(sideIndex == 0 ? "Wing L" : "Wing R", { sideSign * 0.38f, -0.58f, 0.0f }, 0.14f);
        wing.density = 2.5f;
        wing.temperature = 0.8f;
        wing.velocity = { sideSign * 0.7f, 0.7f, 0.0f };
        wing.duration = 0.4f;
        wing.noise = 0.8f;
        wing.colorKey = 0.6f;
    }
    FluidForce& bloom = AddForce("Bloom", FluidForceType::Repulse, 1.2f);
    bloom.center = { 0.0f, -0.45f, 0.0f };
    bloom.radius = 0.8f;
    bloom.falloffPower = 1.3f;
    bloom.duration = 0.45f;
    FluidForce& roll = AddForce("Roll", FluidForceType::Vortex, 0.8f);
    roll.center = { 0.0f, 0.05f, 0.0f };
    roll.direction = { 0.0f, 0.0f, 1.0f };
    roll.radius = 0.9f;
    roll.falloffPower = 1.5f;
    FluidForce& breakUp = AddForce("Break Up", FluidForceType::Noise, 1.2f);
    breakUp.noiseFrequency = 2.5f;
    breakUp.noiseSpeed = 0.8f;
    gasSettings.buoyancy = 1.1f;
    gasSettings.weight = 0.02f;
    gasSettings.vorticity = 0.55f;
    gasSettings.turbulence = 0.35f;
    gasSettings.turbulenceScale = 3.5f;
    gasSettings.densityDissipation = 0.14f;
    gasSettings.temperatureDissipation = 0.9f;
    gasSettings.velocityDamping = 0.12f;
    renderSettings.shading = FluidShading::Smoke;
    renderSettings.useAlbedoRamp = true;
    /// @note 鍵 0 = 芯の濃い灰、鍵 1 = 外周の明るい灰。混ざる境目が «厚み» に見える。
    renderSettings.albedoRamp = { { FluidColorStop{ { 0.16f, 0.16f, 0.17f }, 0.0f },
                          FluidColorStop{ { 0.24f, 0.24f, 0.25f }, 0.33f },
                          FluidColorStop{ { 0.36f, 0.36f, 0.38f }, 0.66f },
                          FluidColorStop{ { 0.50f, 0.50f, 0.53f }, 1.0f } } };
    outputSettings.duration = 2.5f;
    outputSettings.substeps = 3;
}

void FluidPresetBuilder::ConfigureFire()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 燃料だけを流し込み、燃えた所に熱と少しの煤が生まれる。炎の長さは Cooling で決まる。
    /// @note 口を 1 つにすると «細い蝋燭» になる。中央の柱に左右の舌と根元の輪を足して幅を作り、
    /// @note 逆向きの渦 2 つで舌を内側へ巻き込む。煙は注がない (煤は燃焼からしか生まれない)。
    FluidSource& column = AddConeSource("Fuel Column", { 0.0f, -0.95f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.24f, 0.6f);
    column.density = 0.0f;
    column.fuel = 6.5f;
    column.temperature = 2.2f;
    column.velocity = { 0.0f, 1.6f, 0.0f };
    column.noise = 0.9f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& flare = AddConeSource(sideIndex == 0 ? "Flare L" : "Flare R", { sideSign * 0.34f, -0.9f, 0.0f },
                                     { sideSign * 0.3f, 1.0f, 0.0f }, 0.13f, 0.42f);
        flare.density = 0.0f;
        flare.fuel = 3.0f;
        flare.temperature = 1.6f;
        flare.velocity = { sideSign * 0.35f, 1.4f, 0.0f };
        flare.noise = 1.0f;
    }
    FluidSource& embers = AddRingSource("Ember Ring", { 0.0f, -0.82f, 0.0f }, 0.42f, 0.08f);
    embers.density = 0.0f;
    embers.fuel = 1.8f;
    embers.temperature = 1.4f;
    embers.noise = 1.0f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidForce& curl = AddForce(sideIndex == 0 ? "Curl L" : "Curl R", FluidForceType::Vortex, sideSign * -1.1f);
        curl.center = { sideSign * 0.3f, -0.25f, 0.0f };
        curl.direction = { 0.0f, 0.0f, 1.0f };
        curl.radius = 0.55f;
    }
    FluidForce& flicker = AddForce("Flicker", FluidForceType::Noise, 1.4f);
    flicker.noiseFrequency = 4.0f;
    flicker.noiseSpeed = 1.5f;
    gasSettings.buoyancy = 1.8f;
    gasSettings.weight = 0.0f;
    gasSettings.vorticity = 0.6f;
    gasSettings.turbulence = 0.45f;
    gasSettings.turbulenceScale = 4.0f;
    gasSettings.densityDissipation = 0.5f;
    gasSettings.temperatureDissipation = 1.8f;
    gasSettings.ignitionTemperature = 0.2f;
    gasSettings.burnRate = 6.0f;
    gasSettings.burnHeat = 1.3f;
    gasSettings.burnSmoke = 0.6f;
    gasSettings.burnExpansion = 0.6f;
    renderSettings.shading = FluidShading::Fire;
    renderSettings.smokeColor = { 0.25f, 0.23f, 0.22f, 1.0f };
    renderSettings.shadowColor = { 0.05f, 0.04f, 0.04f, 1.0f };
    renderSettings.opacity = 2.6f;
    renderSettings.selfShadow = 1.5f;
    renderSettings.fireKelvin = 1400.0f;
    outputSettings.duration = 1.5f;
    outputSettings.warmup = 1.0f;
    outputSettings.substeps = 4;
    outputSettings.loop = true;
    /// @note 3D の黒体は温度 1 を blackbodyMaxKelvin に当てる。芯だけが白く、先端が暗い赤に落ちる幅にする。
    m_recipe.bake.blackbodyEmission = true;
    m_recipe.bake.blackbodyMinKelvin = 900.0f;
    m_recipe.bake.blackbodyMaxKelvin = 2200.0f;
}

void FluidPresetBuilder::ConfigureExplosion()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 一瞬で大量の燃料を燃やす。膨張が圧力解法を押し広げて火球と爆風になる。
    FluidSource& core = AddSphericalSource("Core", { 0.0f, -0.1f, 0.0f }, 0.2f);
    core.density = 0.8f;
    core.fuel = 10.0f;
    core.temperature = 2.6f;
    core.duration = 0.14f;
    core.noise = 0.7f;
    /// @note 火球の周りに明るい土煙の輪を置く (2D の断面では芯の左右 2 か所)。芯の煤と色の鍵を分け、
    /// @note 黒い芯と灰茶の外側が混ざる境目を albedo_ramp で描く (発生源ごとの色の見本)。
    /// @note 燃料は持たせない: 燃えると膨張で芯より先に爆ぜ、火球の形が崩れる。
    FluidSource& dust = AddRingSource("Dust Ring", { 0.0f, -0.14f, 0.0f }, 0.38f, 0.11f);
    dust.density = 3.2f;
    dust.temperature = 0.4f;
    dust.duration = 0.25f;
    dust.noise = 0.85f;
    dust.colorKey = 1.0f;
    /// @note 火球より一回り外を遅れて走る «衝撃の輪» と、そこから千切れて飛ぶ塊。芯の煙が届く前に
    /// @note 画面の端まで «何かが来た» と分かる。
    FluidSource& shock = AddRingSource("Shock Ring", { 0.0f, -0.1f, 0.0f }, 0.6f, 0.08f);
    shock.density = 1.6f;
    shock.temperature = 0.2f;
    shock.startTime = 0.12f;
    shock.duration = 0.28f;
    shock.noise = 0.9f;
    shock.colorKey = 0.66f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& debris =
            AddSphericalSource(sideIndex == 0 ? "Debris L" : "Debris R", { sideSign * 0.52f, -0.18f, 0.0f }, 0.12f);
        debris.density = 1.6f;
        debris.temperature = 0.3f;
        debris.velocity = { sideSign * 1.2f, 0.5f, 0.0f };
        debris.duration = 0.2f;
        debris.noise = 0.9f;
        debris.colorKey = 0.33f;
    }
    FluidForce& blast = AddForce("Blast", FluidForceType::Repulse, 2.6f);
    blast.center = { 0.0f, -0.1f, 0.0f };
    blast.radius = 0.9f;
    blast.falloffPower = 1.2f;
    blast.duration = 0.3f;
    FluidForce& churn = AddForce("Churn", FluidForceType::Vortex, 1.2f);
    churn.direction = { 0.0f, 0.0f, 1.0f };
    churn.radius = 1.0f;
    FluidForce& shred = AddForce("Shred", FluidForceType::Noise, 1.5f);
    shred.noiseFrequency = 3.5f;
    shred.noiseSpeed = 1.2f;
    renderSettings.useAlbedoRamp = true;
    /// @note リニア。鍵 0 = 芯の煤 (smoke_color より一段暗い)、鍵 1 = 外側の土煙。
    renderSettings.albedoRamp = { { FluidColorStop{ { 0.050f, 0.045f, 0.040f }, 0.0f },
                          FluidColorStop{ { 0.125f, 0.105f, 0.087f }, 0.33f },
                          FluidColorStop{ { 0.200f, 0.165f, 0.133f }, 0.66f },
                          FluidColorStop{ { 0.280f, 0.230f, 0.180f }, 1.0f } } };
    gasSettings.buoyancy = 1.2f;
    gasSettings.weight = 0.02f;
    gasSettings.vorticity = 0.7f;
    gasSettings.turbulence = 0.35f;
    gasSettings.densityDissipation = 0.25f;
    gasSettings.temperatureDissipation = 1.5f;
    gasSettings.ignitionTemperature = 0.2f;
    gasSettings.burnRate = 9.0f;
    gasSettings.burnHeat = 1.8f;
    gasSettings.burnSmoke = 0.9f;
    gasSettings.burnExpansion = 3.0f;
    renderSettings.shading = FluidShading::Fire;
    renderSettings.smokeColor = { 0.32f, 0.29f, 0.27f, 1.0f };
    renderSettings.shadowColor = { 0.06f, 0.05f, 0.05f, 1.0f };
    renderSettings.opacity = 5.0f;
    renderSettings.fireKelvin = 1700.0f;
    outputSettings.duration = 2.2f;
    outputSettings.substeps = 4;
    m_recipe.bake.blackbodyEmission = true;
    m_recipe.bake.blackbodyMinKelvin = 1000.0f;
    m_recipe.bake.blackbodyMaxKelvin = 2600.0f;
}

void FluidPresetBuilder::ConfigureSteam()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 細い口から噴き上がる蒸気。円錐は口で細く、上へ開くので «噴く» 形が球より出やすい。
    /// @note 円錐の体積は同じ幅の球より小さい。注ぐ量が減って薄くならないよう密度を少し上げてある。
    FluidSource& steamJet = AddConeSource("Jet", { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.2f, 0.55f);
    steamJet.density = 5.0f;
    steamJet.temperature = 1.2f;
    steamJet.velocity = { 0.0f, 2.0f, 0.0f };
    steamJet.noise = 0.7f;
    /// @note 柱 1 本だと画面の真ん中に細い線が立つだけ。斜めの口を左右に足して裾を広げ、
    /// @note 高い所へ «雲の頭» を別に湧かせて repulse で潰す (上るほど太る蒸気の形)。
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& vent = AddConeSource(sideIndex == 0 ? "Vent L" : "Vent R", { sideSign * 0.3f, -0.95f, 0.0f },
                                    { sideSign * 0.3f, 1.0f, 0.0f }, 0.13f, 0.38f);
        vent.density = 3.0f;
        vent.temperature = 0.8f;
        vent.velocity = { sideSign * 0.55f, 1.7f, 0.0f };
        vent.noise = 0.8f;
    }
    FluidSource& cap = AddSphericalSource("Cap", { 0.0f, 0.25f, 0.0f }, 0.24f);
    cap.density = 1.6f;
    cap.temperature = 0.25f;
    cap.noise = 0.9f;
    FluidForce& billow = AddForce("Billow", FluidForceType::Repulse, 1.1f);
    billow.center = { 0.0f, 0.2f, 0.0f };
    billow.radius = 0.8f;
    billow.falloffPower = 1.4f;
    FluidForce& wisps = AddForce("Wisps", FluidForceType::Noise, 1.2f);
    wisps.noiseFrequency = 3.0f;
    gasSettings.buoyancy = 1.1f;
    gasSettings.vorticity = 0.4f;
    gasSettings.turbulence = 0.45f;
    gasSettings.densityDissipation = 0.6f;
    gasSettings.temperatureDissipation = 0.8f;
    renderSettings.shading = FluidShading::Smoke;
    renderSettings.smokeColor = { 0.93f, 0.94f, 0.96f, 1.0f };
    renderSettings.shadowColor = { 0.55f, 0.58f, 0.63f, 1.0f };
    renderSettings.opacity = 2.8f;
    renderSettings.selfShadow = 1.2f;
    outputSettings.duration = 2.0f;
    outputSettings.warmup = 1.5f;
    outputSettings.substeps = 3;
    outputSettings.loop = true;
}

void FluidPresetBuilder::ConfigureDustBurst()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 床に張り付いた爆発 (着地・衝撃の土煙)。横へ這わせるだけだと床に薄い帯が残って終わるので、
    /// @note 端をめくり上げてから浮かせ、立ち上がる壁にする。
    FluidSource& impactSource = AddSphericalSource("Impact", { 0.0f, -0.88f, 0.0f }, 0.2f);
    impactSource.density = 2.5f;
    impactSource.fuel = 7.0f;
    impactSource.temperature = 0.7f;
    impactSource.duration = 0.12f;
    impactSource.noise = 0.8f;
    /// @note 床を «掃く» 円錐を左右へ寝かせ、逆回りの渦で外側の端をめくり上げる。押し出す力だけでは
    /// @note 薄い帯が床を這うだけで終わり、縦に何も無い絵になる。
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& sweep = AddConeSource(sideIndex == 0 ? "Sweep L" : "Sweep R", { sideSign * 0.1f, -0.9f, 0.0f },
                                     { sideSign * 1.0f, 0.18f, 0.0f }, 0.16f, 0.7f);
        sweep.density = 4.6f;
        sweep.temperature = 0.25f;
        sweep.velocity = { sideSign * 2.2f, 0.4f, 0.0f };
        sweep.duration = 0.35f;
        sweep.noise = 0.85f;
        sweep.colorKey = 1.0f;
    }
    FluidSource& curlRing = AddRingSource("Curl Ring", { 0.0f, -0.8f, 0.0f }, 0.58f, 0.12f);
    curlRing.density = 3.0f;
    curlRing.temperature = 0.2f;
    curlRing.startTime = 0.15f;
    curlRing.duration = 0.3f;
    curlRing.noise = 0.9f;
    curlRing.colorKey = 0.6f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& billow =
            AddSphericalSource(sideIndex == 0 ? "Billow L" : "Billow R", { sideSign * 0.5f, -0.6f, 0.0f }, 0.18f);
        billow.density = 3.2f;
        billow.temperature = 0.5f;
        billow.velocity = { sideSign * 0.5f, 0.6f, 0.0f };
        billow.startTime = 0.2f;
        billow.duration = 0.35f;
        billow.noise = 0.9f;
        billow.colorKey = 0.35f;
    }
    FluidForce& shove = AddForce("Shove", FluidForceType::Repulse, 3.5f);
    shove.center = { 0.0f, -0.9f, 0.0f };
    shove.radius = 1.1f;
    shove.falloffPower = 1.0f;
    shove.duration = 0.3f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidForce& roll = AddForce(sideIndex == 0 ? "Roll L" : "Roll R", FluidForceType::Vortex, sideSign * 3.0f);
        roll.center = { sideSign * 0.6f, -0.75f, 0.0f };
        roll.direction = { 0.0f, 0.0f, 1.0f };
        roll.radius = 0.8f;
    }
    FluidForce& grit = AddForce("Grit", FluidForceType::Noise, 1.2f);
    grit.noiseFrequency = 3.0f;
    grit.noiseSpeed = 0.7f;
    gasSettings.floor = true;
    gasSettings.buoyancy = 1.35f;
    gasSettings.weight = 0.06f;
    gasSettings.vorticity = 0.5f;
    gasSettings.turbulence = 0.3f;
    gasSettings.densityDissipation = 0.06f;
    gasSettings.temperatureDissipation = 1.2f;
    gasSettings.ignitionTemperature = 0.1f;
    gasSettings.burnRate = 10.0f;
    gasSettings.burnHeat = 0.3f;
    gasSettings.burnSmoke = 2.4f;
    gasSettings.burnExpansion = 4.0f;
    renderSettings.shading = FluidShading::Smoke;
    renderSettings.smokeColor = { 0.66f, 0.56f, 0.43f, 1.0f };
    renderSettings.shadowColor = { 0.22f, 0.17f, 0.12f, 1.0f };
    renderSettings.opacity = 3.5f;
    renderSettings.selfShadow = 2.0f;
    renderSettings.useAlbedoRamp = true;
    /// @note 鍵 0 = 焦げた芯、鍵 1 = 掃き出された乾いた土。
    renderSettings.albedoRamp = { { FluidColorStop{ { 0.070f, 0.055f, 0.040f }, 0.0f },
                          FluidColorStop{ { 0.180f, 0.140f, 0.095f }, 0.33f },
                          FluidColorStop{ { 0.320f, 0.245f, 0.160f }, 0.66f },
                          FluidColorStop{ { 0.480f, 0.370f, 0.240f }, 1.0f } } };
    outputSettings.duration = 2.0f;
    outputSettings.substeps = 3;
}

void FluidPresetBuilder::ConfigureInk()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 浮力なし・高い渦度。噴き出した勢いだけで巻き込む (水中のインク・霊気の渦)。
    /// @note 噴き出し口を横へ振る。動く速さが流れに乗り (inherit_velocity)、筋が片側へ巻き込む。
    /// @note 上下から逆向きに撃ち合わせるのは «ぶつかった所で巻く» ため。1 本だけだと筋が真上へ伸びて
    /// @note 途中で力尽き、タイルの半分が空のまま終わる。
    FluidSource& nozzle = AddSphericalSource("Nozzle", { 0.0f, -0.7f, 0.0f }, 0.1f);
    nozzle.density = 8.0f;
    nozzle.temperature = 0.0f;
    nozzle.velocity = { 0.0f, 2.4f, 0.0f };
    nozzle.duration = 0.8f;
    nozzle.noise = 0.4f;
    nozzle.motion.inheritVelocity = true;
    nozzle.motion.keys = { FluidMotionKey{ 0.0f, { -0.25f, 0.0f, 0.0f } },
                           FluidMotionKey{ 0.8f, { 0.28f, 0.0f, 0.0f } } };
    FluidSource& counter = AddSphericalSource("Counter Nozzle", { 0.0f, 0.7f, 0.0f }, 0.1f);
    counter.density = 6.0f;
    counter.temperature = 0.0f;
    counter.velocity = { 0.0f, -2.0f, 0.0f };
    counter.duration = 0.8f;
    counter.noise = 0.4f;
    counter.colorKey = 1.0f;
    counter.motion.inheritVelocity = true;
    counter.motion.keys = { FluidMotionKey{ 0.0f, { 0.25f, 0.0f, 0.0f } },
                            FluidMotionKey{ 0.8f, { -0.28f, 0.0f, 0.0f } } };
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& bloom = AddSphericalSource(sideIndex == 0 ? "Bloom L" : "Bloom R", { sideSign * 0.55f, 0.0f, 0.0f }, 0.12f);
        bloom.density = 4.0f;
        bloom.temperature = 0.0f;
        bloom.velocity = { sideSign * 1.1f, sideSign * -0.3f, 0.0f };
        bloom.startTime = 0.25f;
        bloom.duration = 0.4f;
        bloom.noise = 0.6f;
        bloom.colorKey = 0.5f;
    }
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidForce& curl = AddForce(sideIndex == 0 ? "Curl L" : "Curl R", FluidForceType::Vortex, sideSign * -2.2f);
        curl.center = { sideSign * 0.35f, 0.0f, 0.0f };
        curl.direction = { 0.0f, 0.0f, 1.0f };
        curl.radius = 0.8f;
        curl.falloffPower = 1.5f;
    }
    FluidForce& feather = AddForce("Feather", FluidForceType::Noise, 1.0f);
    feather.noiseFrequency = 2.5f;
    feather.noiseSpeed = 0.5f;
    gasSettings.buoyancy = 0.0f;
    gasSettings.weight = 0.0f;
    gasSettings.vorticity = 1.0f;
    gasSettings.turbulence = 0.2f;
    gasSettings.densityDissipation = 0.03f;
    gasSettings.temperatureDissipation = 0.0f;
    gasSettings.velocityDamping = 0.35f;
    renderSettings.shading = FluidShading::Smoke;
    renderSettings.smokeColor = { 0.10f, 0.12f, 0.30f, 1.0f };
    renderSettings.shadowColor = { 0.02f, 0.02f, 0.06f, 1.0f };
    renderSettings.opacity = 6.0f;
    renderSettings.selfShadow = 0.6f;
    renderSettings.detailStrength = 0.2f;
    renderSettings.useAlbedoRamp = true;
    /// @note 鍵 0 = 下から撃つ藍、鍵 1 = 上から落ちる紫。ぶつかった所で 2 色が混ざる。
    renderSettings.albedoRamp = { { FluidColorStop{ { 0.030f, 0.045f, 0.140f }, 0.0f },
                          FluidColorStop{ { 0.070f, 0.060f, 0.180f }, 0.33f },
                          FluidColorStop{ { 0.130f, 0.055f, 0.200f }, 0.66f },
                          FluidColorStop{ { 0.210f, 0.060f, 0.190f }, 1.0f } } };
    outputSettings.duration = 3.0f;
    outputSettings.substeps = 3;
}

void FluidPresetBuilder::ConfigureMagicWisp()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 柱 + 輪 + 周回する 2 つの玉。玉は motion のキーで «湧き口ごと» 回るので、
    /// @note 力で回すのと違って輪郭が保たれたまま動く。
    FluidSource& wisp = AddSphericalSource("Wisp", { 0.0f, -0.6f, 0.0f }, 0.16f);
    wisp.density = 3.5f;
    wisp.temperature = 1.4f;
    wisp.velocity = { 0.0f, 0.9f, 0.0f };
    wisp.noise = 0.9f;
    FluidSource& halo = AddRingSource("Halo", { 0.0f, -0.1f, 0.0f }, 0.55f, 0.1f);
    halo.density = 2.0f;
    halo.temperature = 1.0f;
    halo.noise = 1.0f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& mote = AddSphericalSource(sideIndex == 0 ? "Mote L" : "Mote R", { sideSign * 0.5f, -0.35f, 0.0f }, 0.1f);
        mote.density = 1.8f;
        mote.temperature = 0.8f;
        mote.noise = 1.0f;
        mote.motion.inheritVelocity = true;
        /// @note 端と端が同じ位置なので、ループの継ぎ目で玉が跳ばない。
        mote.motion.keys = { FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                             FluidMotionKey{ 1.75f, { sideSign * -0.25f, 0.75f, 0.0f } },
                             FluidMotionKey{ 3.5f, { 0.0f, 0.0f, 0.0f } } };
    }
    /// @note 立ち上る柱をゆっくりねじる渦。半径の外では効かないので形は崩さない。
    FluidForce& swirl = AddForce("Swirl", FluidForceType::Vortex, 1.6f);
    swirl.center = { 0.0f, -0.1f, 0.0f };
    swirl.direction = { 0.0f, 1.0f, 0.0f };
    swirl.radius = 0.9f;
    swirl.falloffPower = 2.0f;
    FluidForce& bloom = AddForce("Bloom", FluidForceType::Repulse, 0.9f);
    bloom.center = { 0.0f, -0.1f, 0.0f };
    bloom.radius = 0.9f;
    bloom.falloffPower = 1.6f;
    FluidForce& shimmer = AddForce("Shimmer", FluidForceType::Noise, 1.8f);
    shimmer.noiseFrequency = 4.5f;
    shimmer.noiseSpeed = 1.2f;
    gasSettings.buoyancy = 0.9f;
    gasSettings.vorticity = 1.0f;
    gasSettings.turbulence = 0.7f;
    gasSettings.turbulenceScale = 5.0f;
    gasSettings.densityDissipation = 0.45f;
    gasSettings.temperatureDissipation = 0.8f;
    renderSettings.shading = FluidShading::Glow;
    renderSettings.smokeColor = { 0.35f, 0.7f, 1.0f, 1.0f };
    renderSettings.opacity = 3.2f;
    renderSettings.useEmissionRamp = true;
    /// @note 冷えた縁の藍から芯の白へ。HDR (1 を超える) のままにして Bloom を掛ける。
    renderSettings.emissionRamp = { { FluidColorStop{ { 0.02f, 0.05f, 0.18f }, 0.0f },
                            FluidColorStop{ { 0.10f, 0.55f, 1.60f }, 0.35f },
                            FluidColorStop{ { 0.45f, 1.60f, 2.60f }, 0.7f },
                            FluidColorStop{ { 2.40f, 3.40f, 4.20f }, 1.0f } } };
    outputSettings.duration = 2.0f;
    outputSettings.warmup = 1.5f;
    outputSettings.substeps = 3;
    outputSettings.loop = true;
}

void FluidPresetBuilder::ConfigureHeatHaze()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 見えない熱気の流れを歪みマップへ。密度は «どこが揺らぐか» のマスクとしてだけ使う。
    /// @note 歪みは色を持たないので、揺れる «面積» がそのまま効き目になる。細い柱ではなく
    /// @note 床いっぱいの熱の床から立ち上げる。
    FluidSource& heatBed = AddSphericalSource("Heat Bed", { 0.0f, -0.85f, 0.0f }, 0.7f);
    heatBed.shape = FluidSourceShape::Box;
    heatBed.size = { 0.7f, 0.12f, 0.3f };
    heatBed.density = 2.6f;
    heatBed.temperature = 1.2f;
    heatBed.velocity = { 0.0f, 0.8f, 0.0f };
    heatBed.noise = 0.7f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& column = AddConeSource(sideIndex == 0 ? "Column L" : "Column R", { sideSign * 0.5f, -0.9f, 0.0f },
                                      { sideSign * 0.2f, 1.0f, 0.0f }, 0.18f, 0.6f);
        column.density = 2.0f;
        column.temperature = 1.0f;
        column.velocity = { sideSign * 0.3f, 1.1f, 0.0f };
        column.noise = 0.9f;
    }
    FluidForce& shimmer = AddForce("Shimmer", FluidForceType::Noise, 2.2f);
    shimmer.noiseFrequency = 5.0f;
    shimmer.noiseSpeed = 1.4f;
    FluidForce& drift = AddForce("Drift", FluidForceType::Wind, 0.5f);
    drift.direction = { 1.0f, 0.15f, 0.0f };
    FluidForce& spread = AddForce("Spread", FluidForceType::Repulse, 0.8f);
    spread.center = { 0.0f, -0.1f, 0.0f };
    spread.radius = 1.0f;
    spread.falloffPower = 1.4f;
    gasSettings.buoyancy = 1.6f;
    gasSettings.vorticity = 0.5f;
    gasSettings.turbulence = 0.6f;
    gasSettings.turbulenceScale = 4.0f;
    gasSettings.densityDissipation = 0.35f;
    renderSettings.shading = FluidShading::Distortion;
    renderSettings.opacity = 2.5f;
    outputSettings.duration = 2.0f;
    outputSettings.warmup = 1.5f;
    outputSettings.substeps = 3;
    outputSettings.loop = true;
    outputSettings.motionVectors = false;
}

void FluidPresetBuilder::ConfigureWaterSplash()
{
    FluidLiquidSettings& liquidSettings = m_recipe.liquid;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    ConfigureLiquidDefaults();

    /// @note 芯の柱 + 左右の王冠 + 上から降る霧。spread (ばらつき) は «撃ち出す速さ × spread» の
    /// @note 横向きの当たりくじで、1 秒あれば当たった粒はタイルの外へ出るため小さく保ち、横幅は
    /// @note 別の湧き口で作る。寿命を付けるのは、床に落ちた粒の溜まりが «粒子の数 ÷ 高さ» の幅まで
    /// @note 必ず広がるため。飛沫が消えていけば、溜まりが枠を越える前に絵が終わる。
    FluidSource& splashSource = AddSphericalSource("Splash", { 0.0f, -0.85f, 0.0f }, 0.15f);
    splashSource.velocity = { 0.0f, 3.6f, 0.0f };
    splashSource.spread = 0.16f;
    splashSource.count = 560;
    splashSource.duration = 0.16f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& crown =
            AddSphericalSource(sideIndex == 0 ? "Crown L" : "Crown R", { sideSign * 0.42f, -0.9f, 0.0f }, 0.14f);
        crown.velocity = { 0.0f, 2.4f, 0.0f };
        crown.spread = 0.15f;
        crown.count = 90;
        crown.startTime = 0.1f;
        crown.duration = 0.35f;
    }
    FluidSource& mist = AddSphericalSource("Mist", { 0.0f, 0.45f, 0.0f }, 0.55f);
    mist.velocity = { 0.0f, -0.2f, 0.0f };
    mist.spread = 0.3f;
    mist.count = 110;
    mist.startTime = 0.15f;
    mist.duration = 0.5f;
    liquidSettings.particleRadius = 0.011f;
    liquidSettings.gravity = 7.0f;
    liquidSettings.viscosity = 0.02f;
    liquidSettings.cohesion = 0.25f;
    liquidSettings.solverIterations = 6;
    liquidSettings.floorHeight = -0.95f;
    liquidSettings.floorFriction = 0.95f;
    liquidSettings.particleLifetime = 0.85f;
    renderSettings.liquidColor = { 0.40f, 0.65f, 0.95f, 0.8f };
    renderSettings.specular = 0.9f;
    outputSettings.duration = 1.1f;
}

void FluidPresetBuilder::ConfigureWaterJet()
{
    FluidLiquidSettings& liquidSettings = m_recipe.liquid;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    ConfigureLiquidDefaults();

    FluidSource& jetSource = AddSphericalSource("Jet", { -0.85f, -0.3f, 0.0f }, 0.04f);
    jetSource.velocity = { 2.4f, 1.4f, 0.0f };
    jetSource.spread = 0.08f;
    jetSource.count = 1600;
    jetSource.duration = 1.6f;
    /// @note 芯の弧に «ばらけた霧» を重ねて 1 本の線に見えないようにする。ばらつきは小さく保つ
    /// @note (大きくすると後ろ向きに飛ぶ粒が出て、口の左側がタイルからはみ出す)。
    FluidSource& spray = AddSphericalSource("Spray", { -0.82f, -0.28f, 0.0f }, 0.07f);
    spray.velocity = { 2.1f, 1.5f, 0.0f };
    spray.spread = 0.22f;
    spray.count = 600;
    spray.duration = 1.6f;
    /// @note 岩の天面から跳ね返る水。ほぼ真上に撃つ (横へ撃つと右端から出ていく)。
    FluidSource& rebound = AddSphericalSource("Rebound", { 0.62f, -0.62f, 0.0f }, 0.12f);
    rebound.velocity = { 0.0f, 2.2f, 0.0f };
    rebound.spread = 0.4f;
    rebound.count = 700;
    rebound.startTime = 0.25f;
    rebound.duration = 1.35f;
    /// @note 弧が床の少し上を横切る所 (x ≈ 0.7) に岩を置く。噴流が天面で砕けて両側へこぼれる。
    /// @note 床 (floorHeight = -0.95) に接地させ、下をくぐる隙間を作らない。
    FluidCollider rock;
    rock.name = "Rock";
    rock.shape = FluidColliderShape::Box;
    rock.center = { 0.7f, -0.8f, 0.0f };
    rock.size = { 0.15f, 0.15f, 0.3f };
    m_recipe.colliders.push_back(std::move(rock));
    liquidSettings.gravity = 6.0f;
    liquidSettings.viscosity = 0.03f;
    liquidSettings.cohesion = 0.35f;
    liquidSettings.floorHeight = -0.95f;
    liquidSettings.floorFriction = 0.8f;
    renderSettings.liquidColor = { 0.40f, 0.65f, 0.95f, 0.8f };
    renderSettings.specular = 0.9f;
    outputSettings.duration = 0.8f;
    outputSettings.warmup = 0.8f;
    outputSettings.loop = true;
}

void FluidPresetBuilder::ConfigureBloodBurst()
{
    FluidLiquidSettings& liquidSettings = m_recipe.liquid;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    ConfigureLiquidDefaults();

    /// @note 全方位へ飛ぶ粘い雫。床は置かず、寿命で細りながら消す (被弾の飛沫)。床が無いので、
    /// @note 撃ち出す速さは遅く保つ (速い粒はそのままタイルの外へ出て二度と戻らない)。«大きさ» は
    /// @note 速度ではなく «湧き口の広さ» と «数» で作る (粒子どうしが押し合って広がる)。
    FluidSource& burstSource = AddSphericalSource("Burst", { 0.0f, 0.2f, 0.0f }, 0.26f);
    burstSource.velocity = { 0.0f, 0.3f, 0.0f };
    burstSource.spread = 0.5f;
    burstSource.count = 400;
    burstSource.duration = 0.1f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& spray =
            AddSphericalSource(sideIndex == 0 ? "Spray L" : "Spray R", { sideSign * 0.42f, 0.1f, 0.0f }, 0.14f);
        spray.velocity = { sideSign * 0.45f, 0.25f, 0.0f };
        spray.spread = 0.25f;
        spray.count = 160;
        spray.duration = 0.14f;
    }
    FluidSource& gout = AddSphericalSource("Gout", { 0.0f, -0.05f, 0.0f }, 0.18f);
    gout.velocity = { 0.0f, -0.2f, 0.0f };
    gout.spread = 0.25f;
    gout.count = 160;
    gout.startTime = 0.08f;
    gout.duration = 0.3f;
    liquidSettings.gravity = 0.8f;
    liquidSettings.viscosity = 0.2f;
    liquidSettings.cohesion = 0.6f;
    liquidSettings.floor = false;
    liquidSettings.particleLifetime = 1.1f;
    renderSettings.liquidColor = { 0.42f, 0.02f, 0.03f, 0.95f };
    renderSettings.specular = 0.6f;
    /// @note 3D の液面: 血は濃く (透けない)、艶は水より鈍い。
    renderSettings.liquidExtinction = 120.0f;
    renderSettings.liquidGloss = 64.0f;
    outputSettings.duration = 0.9f;
}

void FluidPresetBuilder::ConfigureLavaBlob()
{
    FluidLiquidSettings& liquidSettings = m_recipe.liquid;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    ConfigureLiquidDefaults();

    /// @note 噴き上げた塊が落ちて、床いっぱいに広がる «溜まり» になる。粒子 1 つは «その面積» を
    /// @note 要求し、床に落ちた溜まりの広さはほぼ «数 × 粒子の断面» で決まるので、粒子を増やすと
    /// @note タイルの左右から溢れる (高さは増えない)。
    FluidSource& lavaSource = AddSphericalSource("Blob", { 0.0f, -0.8f, 0.0f }, 0.12f);
    lavaSource.velocity = { 0.0f, 2.6f, 0.0f };
    lavaSource.spread = 0.3f;
    lavaSource.count = 190;
    lavaSource.duration = 0.15f;
    for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
        const float sideSign = sideIndex == 0 ? -1.0f : 1.0f;
        FluidSource& lob = AddSphericalSource(sideIndex == 0 ? "Lob L" : "Lob R", { sideSign * 0.28f, -0.85f, 0.0f }, 0.1f);
        lob.velocity = { sideSign * 0.25f, 2.5f, 0.0f };
        lob.spread = 0.25f;
        lob.count = 95;
        lob.startTime = 0.05f;
        lob.duration = 0.18f;
    }
    FluidSource& spatter = AddSphericalSource("Spatter", { 0.0f, -0.7f, 0.0f }, 0.16f);
    spatter.velocity = { 0.0f, 1.8f, 0.0f };
    spatter.spread = 0.5f;
    spatter.count = 70;
    spatter.startTime = 0.12f;
    spatter.duration = 0.3f;
    liquidSettings.particleRadius = 0.014f;
    liquidSettings.gravity = 5.0f;
    liquidSettings.viscosity = 0.8f;
    liquidSettings.cohesion = 0.9f;
    liquidSettings.floorHeight = -0.95f;
    liquidSettings.floorFriction = 0.95f;
    renderSettings.liquidColor = { 1.0f, 0.38f, 0.06f, 1.0f };
    renderSettings.specular = 0.5f;
    /// @note 3D の液面: 溶岩は不透明で縁が柔らかい。
    renderSettings.liquidExtinction = 160.0f;
    renderSettings.liquidSoftness = 0.12f;
    renderSettings.liquidGloss = 32.0f;
    outputSettings.duration = 1.4f;
}

void FluidPresetBuilder::ConfigurePlasmaBurst()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 放電が弾けた «瞬間» の空気。稲妻の線そのものは VFX Line の担当で、ここは線の周りだけ。
    /// @note 芯 → 輪 → 上下へ抜ける枝の 3 段で、0.8 秒のうちにタイルの端まで届いて消える。
    FluidSource& core = AddSphericalSource("Core", { 0.0f, 0.0f, 0.0f }, 0.14f);
    core.density = 7.0f;
    core.temperature = 4.0f;
    core.duration = 0.08f;
    core.noise = 0.4f;
    /// @note 輪の法線を奥行き軸にして «こちらを向いた輪» にする (上向きだと真横から見た線になる)。
    FluidSource& shell = AddRingSource("Shell", { 0.0f, 0.0f, 0.0f }, 0.4f, 0.12f);
    shell.direction = { 0.0f, 0.0f, 1.0f };
    shell.density = 4.0f;
    shell.temperature = 2.5f;
    shell.startTime = 0.04f;
    shell.duration = 0.14f;
    shell.noise = 0.9f;
    struct Branch {
        const char* name;
        math::Vector3 direction;
        math::Vector3 velocity;
    };
    /// @note 上下で傾きを揃えない。左右対称だと «X 字» に見えて、枝分かれではなく図形になる。
    const Branch BRANCHES[] = { { "Branch Up", { 0.25f, 1.0f, 0.0f }, { 0.5f, 2.2f, 0.0f } },
                                 { "Branch Down", { -0.3f, -1.0f, 0.0f }, { -0.6f, -2.2f, 0.0f } } };
    for (const Branch& branch : BRANCHES) {
        FluidSource& arm = AddConeSource(branch.name, { 0.0f, 0.0f, 0.0f }, branch.direction, 0.16f, 0.8f);
        arm.density = 3.0f;
        arm.temperature = 2.0f;
        arm.velocity = branch.velocity;
        arm.duration = 0.12f;
        arm.noise = 1.0f;
    }
    FluidForce& discharge = AddForce("Discharge", FluidForceType::Repulse, 7.0f);
    discharge.radius = 1.2f;
    discharge.falloffPower = 0.9f;
    discharge.duration = 0.22f;
    FluidForce& fork = AddForce("Fork", FluidForceType::Noise, 3.0f);
    fork.noiseFrequency = 6.0f;
    fork.noiseSpeed = 2.5f;
    /// @note 押し切ったあとに止める。減速が無いと «弾けた» ではなく «流れ去った» に見える。
    FluidForce& settle = AddForce("Settle", FluidForceType::Drag, 4.0f);
    settle.startTime = 0.3f;
    gasSettings.buoyancy = 0.25f;
    gasSettings.weight = 0.0f;
    gasSettings.vorticity = 0.8f;
    gasSettings.turbulence = 0.5f;
    gasSettings.turbulenceScale = 6.0f;
    gasSettings.densityDissipation = 1.2f;
    gasSettings.temperatureDissipation = 2.6f;
    gasSettings.velocityDamping = 0.6f;
    renderSettings.shading = FluidShading::Glow;
    renderSettings.smokeColor = { 0.55f, 0.75f, 1.0f, 1.0f };
    renderSettings.opacity = 5.0f;
    renderSettings.useEmissionRamp = true;
    /// @note 青 → 白。白の側は 1 を大きく超える HDR にして、Bloom で «放電» の明るさを出す。
    renderSettings.emissionRamp = { { FluidColorStop{ { 0.05f, 0.12f, 0.45f }, 0.0f },
                            FluidColorStop{ { 0.35f, 0.95f, 2.60f }, 0.4f },
                            FluidColorStop{ { 1.80f, 3.20f, 5.50f }, 0.75f },
                            FluidColorStop{ { 5.50f, 6.20f, 7.00f }, 1.0f } } };
    outputSettings.duration = 0.8f;
    outputSettings.substeps = 4;
    outputSettings.loop = false;
}

void FluidPresetBuilder::ConfigureArcHaze()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    /// @note 放電の残り香。細く長く漂う靄。形を崩すのは noise の力で、位置を運ぶのは玉の motion。
    FluidSource& trailSource = AddConeSource("Trail", { 0.0f, -0.95f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.16f, 0.7f);
    trailSource.density = 2.4f;
    trailSource.temperature = 1.2f;
    trailSource.velocity = { 0.0f, 1.1f, 0.0f };
    trailSource.noise = 1.0f;
    FluidSource& veil = AddSphericalSource("Veil", { 0.0f, -0.4f, 0.0f }, 0.55f);
    veil.shape = FluidSourceShape::Box;
    veil.size = { 0.55f, 0.09f, 0.3f };
    veil.density = 1.2f;
    veil.temperature = 0.8f;
    veil.noise = 1.0f;
    struct Ember {
        const char* name;
        float x;
        math::Vector3 rest;
        math::Vector3 swing;
    };
    /// @note 端 (0 と 3.5) が同じ位置なので、ループの継ぎ目で玉が跳ばない。左右は位相を逆に取る。
    const Ember EMBERS[] = { { "Ember L", -0.55f, { 0.0f, -0.15f, 0.0f }, { 0.3f, 0.35f, 0.0f } },
                              { "Ember R", 0.55f, { 0.0f, 0.35f, 0.0f }, { -0.3f, -0.15f, 0.0f } } };
    for (const Ember& ember : EMBERS) {
        FluidSource& mote = AddSphericalSource(ember.name, { ember.x, 0.05f, 0.0f }, 0.12f);
        mote.density = 2.0f;
        mote.temperature = 1.0f;
        mote.noise = 1.0f;
        mote.motion.inheritVelocity = true;
        mote.motion.keys = { FluidMotionKey{ 0.0f, ember.rest },
                             FluidMotionKey{ 1.75f, ember.swing },
                             FluidMotionKey{ 3.5f, ember.rest } };
    }
    FluidForce& tear = AddForce("Tear", FluidForceType::Noise, 2.0f);
    tear.noiseFrequency = 5.5f;
    tear.noiseSpeed = 1.6f;
    FluidForce& spread = AddForce("Spread", FluidForceType::Repulse, 0.5f);
    spread.center = { 0.0f, -0.15f, 0.0f };
    spread.radius = 1.1f;
    spread.falloffPower = 1.2f;
    FluidForce& creep = AddForce("Creep", FluidForceType::Vortex, 0.9f);
    creep.center = { 0.0f, 0.1f, 0.0f };
    creep.direction = { 0.0f, 0.0f, 1.0f };
    creep.radius = 1.0f;
    creep.falloffPower = 1.5f;
    gasSettings.buoyancy = 0.55f;
    gasSettings.weight = 0.0f;
    gasSettings.vorticity = 1.2f;
    gasSettings.turbulence = 0.8f;
    gasSettings.turbulenceScale = 6.0f;
    gasSettings.densityDissipation = 0.85f;
    gasSettings.temperatureDissipation = 0.7f;
    gasSettings.velocityDamping = 0.25f;
    renderSettings.shading = FluidShading::Glow;
    renderSettings.smokeColor = { 0.42f, 0.68f, 1.0f, 1.0f };
    renderSettings.opacity = 4.5f;
    renderSettings.useEmissionRamp = true;
    renderSettings.emissionRamp = { { FluidColorStop{ { 0.02f, 0.06f, 0.20f }, 0.0f },
                            FluidColorStop{ { 0.08f, 0.35f, 1.00f }, 0.4f },
                            FluidColorStop{ { 0.35f, 1.10f, 2.20f }, 0.75f },
                            FluidColorStop{ { 1.60f, 2.40f, 3.60f }, 1.0f } } };
    outputSettings.duration = 2.4f;
    outputSettings.warmup = 1.5f;
    outputSettings.substeps = 3;
    outputSettings.loop = true;
}

void FluidPresetBuilder::ConfigureGroundRing()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    FluidSource& ring = AddRingSource("Ground Front", { 0.0f, -0.35f, 0.0f }, 0.28f, 0.07f);
    ring.direction = { 0.0f, 0.0f, 1.0f };
    ring.density = 12.0f;
    ring.temperature = 0.2f;
    ring.duration = 0.1f;
    FluidForce& spread = AddForce("Impact Expansion", FluidForceType::Repulse, 6.0f);
    spread.center = ring.center;
    spread.radius = 1.5f;
    spread.duration = 0.25f;
    AddForce("Settle", FluidForceType::Drag, 2.0f).startTime = 0.3f;
    gasSettings.buoyancy = 0.0f;
    gasSettings.weight = 0.1f;
    gasSettings.densityDissipation = 1.8f;
    renderSettings.smokeColor = { 0.42f, 0.32f, 0.22f, 1.0f };
    renderSettings.opacity = 4.0f;
    outputSettings.duration = 1.1f;
}

void FluidPresetBuilder::ConfigureColdMist()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    FluidSource& mist = AddSphericalSource("Cold Veil", { 0.0f, -0.55f, 0.0f }, 0.4f);
    mist.shape = FluidSourceShape::Box;
    mist.size = { 0.5f, 0.08f, 0.25f };
    mist.density = 5.0f;
    mist.temperature = 0.0f;
    mist.duration = 0.45f;
    mist.velocity = { 0.0f, 0.25f, 0.0f };
    AddForce("Crawl", FluidForceType::Noise, 0.8f).noiseFrequency = 4.0f;
    FluidForce& spread = AddForce("Spread", FluidForceType::Repulse, 1.0f);
    spread.center = mist.center;
    spread.radius = 1.2f;
    gasSettings.buoyancy = 0.0f;
    gasSettings.weight = 0.1f;
    gasSettings.floor = true;
    gasSettings.densityDissipation = 1.2f;
    renderSettings.smokeColor = { 0.65f, 0.85f, 1.0f, 1.0f };
    renderSettings.opacity = 2.5f;
    outputSettings.duration = 2.0f;
}

void FluidPresetBuilder::ConfigureChargeVortex()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    FluidSource& ring = AddRingSource("Gathering Ring", {}, 0.65f, 0.09f);
    ring.direction = { 0.0f, 0.0f, 1.0f };
    ring.density = 6.0f;
    ring.temperature = 1.0f;
    ring.duration = 0.7f;
    ring.amount.keys = { { 0.0f, 0.1f }, { 0.3f, 1.0f }, { 0.7f, 0.0f } };
    FluidForce& pull = AddForce("Pull Into Core", FluidForceType::Attract, 4.0f);
    pull.radius = 1.4f;
    FluidForce& spin = AddForce("Spiral", FluidForceType::Vortex, 2.5f);
    spin.direction = { 0.0f, 0.0f, 1.0f };
    spin.radius = 1.2f;
    gasSettings.buoyancy = 0.0f;
    gasSettings.weight = 0.0f;
    gasSettings.densityDissipation = 1.4f;
    renderSettings.shading = FluidShading::Glow;
    renderSettings.smokeColor = { 0.25f, 0.55f, 1.0f, 1.0f };
    renderSettings.opacity = 5.0f;
    outputSettings.duration = 1.0f;
}

void FluidPresetBuilder::ConfigureEmberBurst()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    for (int emberIndex = 0; emberIndex < 5; ++emberIndex) {
        const float offsetX = static_cast<float>(emberIndex - 2) * 0.16f;
        const std::string name = "Ember " + std::to_string(emberIndex);
        FluidSource& ember = AddSphericalSource(name.c_str(), { offsetX, -0.4f, 0.0f }, 0.06f);
        ember.density = 8.0f;
        ember.temperature = 3.0f;
        ember.duration = 0.12f;
        ember.velocity = { offsetX * 3.0f, 1.2f + 0.2f * static_cast<float>(emberIndex % 2), 0.0f };
    }
    AddForce("Break Up", FluidForceType::Noise, 1.0f).noiseFrequency = 7.0f;
    gasSettings.buoyancy = 0.2f;
    gasSettings.weight = 0.0f;
    gasSettings.densityDissipation = 2.2f;
    renderSettings.shading = FluidShading::Glow;
    renderSettings.smokeColor = { 1.0f, 0.28f, 0.03f, 1.0f };
    renderSettings.opacity = 6.0f;
    outputSettings.duration = 1.2f;
}

void FluidPresetBuilder::ConfigureSigilFlare()
{
    FluidGasSettings& gasSettings = m_recipe.gas;
    FluidRenderSettings& renderSettings = m_recipe.render;
    FluidOutputSettings& outputSettings = m_recipe.output;

    FluidSource& outer = AddRingSource("Outer Seal", {}, 0.55f, 0.045f);
    outer.direction = { 0.0f, 0.0f, 1.0f };
    outer.density = 12.0f;
    outer.temperature = 2.0f;
    outer.duration = 0.22f;
    FluidSource& inner = AddRingSource("Inner Seal", {}, 0.3f, 0.04f);
    inner.direction = { 0.0f, 0.0f, 1.0f };
    inner.density = 10.0f;
    inner.temperature = 2.0f;
    inner.startTime = 0.08f;
    inner.duration = 0.2f;
    AddForce("Release", FluidForceType::Repulse, 2.0f).startTime = 0.25f;
    AddForce("Fray", FluidForceType::Noise, 0.6f).noiseFrequency = 6.0f;
    gasSettings.buoyancy = 0.0f;
    gasSettings.weight = 0.0f;
    gasSettings.densityDissipation = 2.0f;
    renderSettings.shading = FluidShading::Glow;
    renderSettings.smokeColor = { 0.7f, 0.25f, 1.0f, 1.0f };
    renderSettings.opacity = 6.0f;
    outputSettings.duration = 1.4f;
}

} /// @note namespace

const char* FluidPresetName(FluidPreset preset)
{
    switch (preset) {
    case FluidPreset::Smoke:       return "Smoke Puff";
    case FluidPreset::Fire:        return "Fire (loop)";
    case FluidPreset::Explosion:   return "Explosion";
    case FluidPreset::Steam:       return "Steam (loop)";
    case FluidPreset::DustBurst:   return "Dust Burst";
    case FluidPreset::Ink:         return "Ink Swirl";
    case FluidPreset::MagicWisp:   return "Magic Wisp (loop)";
    case FluidPreset::HeatHaze:    return "Heat Haze (loop)";
    case FluidPreset::WaterSplash: return "Water Splash";
    case FluidPreset::WaterJet:    return "Water Jet (loop)";
    case FluidPreset::BloodBurst:  return "Blood Burst";
    case FluidPreset::LavaBlob:    return "Lava Blob";
    case FluidPreset::PlasmaBurst: return "Plasma Burst";
    case FluidPreset::ArcHaze:     return "Arc Haze (loop)";
    case FluidPreset::GroundRing:  return "Ground Shock Ring";
    case FluidPreset::ColdMist:    return "Cold Mist";
    case FluidPreset::ChargeVortex: return "Charge Vortex";
    case FluidPreset::EmberBurst:  return "Ember Burst";
    case FluidPreset::SigilFlare:  return "Sigil Flare";
    case FluidPreset::Count:
    default:                       return "Unknown";
    }
}

fluid::FluidRecipe MakeFluidPreset(FluidPreset preset)
{
    return FluidPresetBuilder::Build(preset);
}

} /// @note namespace fbzz::asset
