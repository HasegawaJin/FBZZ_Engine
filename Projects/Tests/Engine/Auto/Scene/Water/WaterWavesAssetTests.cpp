/// @file    WaterWavesAssetTests.cpp
/// @brief   水面の波アセットの保存、波の生成と CPU 照会の契約。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Support/TempDir.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Data/DataAssetRegistry.hpp>
#include <Engine/Asset/Water/WaterWavesAsset.hpp>
#include <Engine/Asset/Water/WaterOpticalSettings.hpp>
#include <Graphics/Pipeline/Resources/RenderConstants.hpp>
#include <cstring>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Hierarchy/Transform.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Systems/Water/WaterSystem.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <Physics/World/World.hpp>

#include <filesystem>
#include <array>
#include <cmath>
#include <span>
#include <string>

namespace fbzz::tests {
namespace {

/// @note 本番と同じく、所有された編集データを公開して更新する。
template<typename Owner, typename Change>
void EditMaterialAuthoring(Owner& owner, Change&& change)
{
    auto edit = owner.BeginAuthoringEdit();
    change(edit.Draft());
    EXPECT_NE(edit.Commit(owner), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
}



bool SameWave(const fluid::GerstnerWave& a, const fluid::GerstnerWave& b)
{
    return a.direction.x == b.direction.x && a.direction.y == b.direction.y
        && a.amplitude == b.amplitude && a.wavelength == b.wavelength
        && a.steepness == b.steepness && a.phaseOffsetRadians == b.phaseOffsetRadians;
}

void UpdateWaterSystem(scene::WaterSystem& system, scene::Scene& scene, physics::World& world)
{
    SystemContext context{scene, world, nullptr, nullptr, 1.0f / 60.0f,
                          1.0f / 60.0f, false, false};
    system.Update(context);
}

} /// @note namespace

class WaterWavesAssetTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        asset::DataAssetRegistry::ClearCache();
        std::filesystem::create_directories(m_temp.File("Assets"));
    }

    void TearDown() override
    {
        asset::DataAssetRegistry::ClearCache();
        EngineFixture::TearDown();
    }

    std::string File() const
    {
        return m_temp.File("Assets/Waves.fzdata").generic_string();
    }

    asset::WaterWavesAsset* Resolve()
    {
        asset::DataAsset* data = asset::DataAssetRegistry::Resolve(File());
        if (!data || std::string(data->GetTypeName()) != asset::WaterWavesAsset::TYPE_NAME) {
            return nullptr;
        }
        return static_cast<asset::WaterWavesAsset*>(data);
    }

    testkit::TempDir m_temp{"WaterWavesAsset"};
};

/// @brief 組み込みアセットの反射値を保存し、同じ共有実体へ再ロードする。
TEST_F(WaterWavesAssetTest, BuiltinAssetRoundTripsReflectedValuesAndReloadsTheSharedInstance)
{
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::WaterWavesAsset::TYPE_NAME));
    asset::WaterWavesAsset* waves = Resolve();
    ASSERT_NE(waves, nullptr);
    EXPECT_EQ(waves->waveCount, 8);
    EXPECT_FLOAT_EQ(waves->waveSpread, 0.0f);
    waves->seed = 821;
    waves->waveCount = 11;
    waves->minimumWavelength = 7.0f;
    waves->maximumWavelength = 31.0f;
    waves->minimumAmplitude = 0.12f;
    waves->maximumAmplitude = 0.38f;
    waves->directionCenterRadians = 0.72f;
    waves->directionSpreadRadians = 0.4f;
    waves->minimumSteepness = 0.21f;
    waves->maximumSteepness = 0.49f;
    waves->minimumPhaseOffsetRadians = 0.1f;
    waves->maximumPhaseOffsetRadians = 2.8f;
    waves->waveGrouping = 0.3f;
    waves->waveSpread = 0.0f;
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> expected{};
    ASSERT_TRUE(waves->GenerateWaves(expected));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));

    waves->seed = 2;
    waves->waveCount = 1;
    ASSERT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    EXPECT_EQ(Resolve(), waves);
    EXPECT_EQ(waves->seed, 821);
    EXPECT_EQ(waves->waveCount, 11);
    EXPECT_FLOAT_EQ(waves->minimumWavelength, 7.0f);
    EXPECT_FLOAT_EQ(waves->maximumWavelength, 31.0f);
    EXPECT_FLOAT_EQ(waves->minimumAmplitude, 0.12f);
    EXPECT_FLOAT_EQ(waves->maximumAmplitude, 0.38f);
    EXPECT_FLOAT_EQ(waves->directionCenterRadians, 0.72f);
    EXPECT_FLOAT_EQ(waves->directionSpreadRadians, 0.4f);
    EXPECT_FLOAT_EQ(waves->minimumSteepness, 0.21f);
    EXPECT_FLOAT_EQ(waves->maximumSteepness, 0.49f);
    EXPECT_FLOAT_EQ(waves->minimumPhaseOffsetRadians, 0.1f);
    EXPECT_FLOAT_EQ(waves->maximumPhaseOffsetRadians, 2.8f);
    EXPECT_FLOAT_EQ(waves->waveGrouping, 0.3f);
    EXPECT_FLOAT_EQ(waves->waveSpread, 0.0f);
    std::array<fluid::GerstnerWave, fluid::MAX_BASE_GERSTNER_WAVES> reloaded{};
    ASSERT_TRUE(waves->GenerateWaves(reloaded));
    for (size_t i = 0; i < static_cast<size_t>(waves->waveCount); ++i)
        EXPECT_TRUE(SameWave(reloaded[i], expected[i]));
}

/// @brief アセット由来の基底波 8 本を保持し、振幅倍率を適用した後の収縮率を制限する。
TEST_F(WaterWavesAssetTest, AssetResolutionKeepsEightBaseWavesAndLimitsScaledContraction)
{
    asset::WaterWavesAsset source;
    source.waveCount = 8;
    source.minimumWavelength = source.maximumWavelength = 10.0f;
    source.minimumAmplitude = source.maximumAmplitude = 0.6f;
    source.directionSpreadRadians = 0.0f;
    source.minimumSteepness = source.maximumSteepness = 0.9f;
    source.waveGrouping = 0.45f;
    source.waveSpread = 0.0f;
    asset::MaterialAsset material;
    EditMaterialAuthoring(material, [&](auto& values) { values.params[scene::water_keys::kWindResponse] = {1.0f}; });
    EditMaterialAuthoring(material, [&](auto& values) { values.params[scene::water_keys::kWaveAmplitude[0]] = {9.0f}; });
    EditMaterialAuthoring(material, [&](auto& values) { values.params[scene::water_keys::kFlowDirection] = {0.0f, 1.0f}; });
    EditMaterialAuthoring(material, [&](auto& values) { values.params[scene::water_keys::kCurrentSpeed] = {2.0f}; });
    scene::WaterWind wind{true, {1.0f, 0.0f, 0.0f}, 10.0f};
    scene::WaterComponent water;
    water.waveAmplitudeScale = 1.7f;
    ASSERT_TRUE(scene::ResolveWaterWavesFromAsset(water, &material, wind, source));
    EXPECT_EQ(water.waveCount, 8u);
    EXPECT_NEAR(water.waves[0].amplitude, 1.02f, 1.0e-5f);
    EXPECT_FLOAT_EQ(water.waveSpread, 0.0f);
    EXPECT_NEAR(water.current.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(water.current.y, 2.0f, 1.0e-6f);
    EXPECT_LE(fluid::GerstnerWaveContractionBound(
        std::span<const fluid::GerstnerWave>(water.waves.data(), water.waveCount),
        water.waveGrouping, water.waveSpread), 0.95f);
}

/// @brief アセット設定が不正なら解決済みの波を変更しない。
TEST_F(WaterWavesAssetTest, InvalidAssetSettingsFailWithoutChangingTheResolvedWaveSource)
{
    asset::WaterWavesAsset source;
    source.waveCount = static_cast<int>(fluid::MAX_BASE_GERSTNER_WAVES + 1);
    scene::WaterComponent water;
    water.waveCount = 3;
    water.waves[0] = {{0.2f, 0.8f}, 1.25f, 14.0f, 0.4f, 0.7f};
    const fluid::GerstnerWave before = water.waves[0];
    EXPECT_FALSE(scene::ResolveWaterWavesFromAsset(water, nullptr, {}, source));
    EXPECT_EQ(water.waveCount, 3u);
    EXPECT_FLOAT_EQ(water.waves[0].direction.x, before.direction.x);
    EXPECT_FLOAT_EQ(water.waves[0].direction.y, before.direction.y);
    EXPECT_FLOAT_EQ(water.waves[0].amplitude, before.amplitude);
    EXPECT_FLOAT_EQ(water.waves[0].wavelength, before.wavelength);
    EXPECT_FLOAT_EQ(water.waves[0].steepness, before.steepness);
    EXPECT_FLOAT_EQ(water.waves[0].phaseOffsetRadians, before.phaseOffsetRadians);
}

/// @brief 同じ材質を使う水面ごとの倍率を保持し、公開値の編集を即時反映する。
TEST_F(WaterWavesAssetTest, SharedMaterialPublicationKeepsPerWaterScaleAndImmediateEdits)
{
    asset::MaterialAsset material;
    EditMaterialAuthoring(material, [](auto& values) {
        values.params[scene::water_keys::kWaveDirection[0]] = {1.0f, 0.0f};
        values.params[scene::water_keys::kWaveAmplitude[0]] = {1.0f};
        values.params[scene::water_keys::kFlowDirection] = {3.0f, 4.0f};
        values.params[scene::water_keys::kCurrentSpeed] = {5.0f};
    });
    const auto retained = material.ReadAuthoring();
    scene::WaterComponent first;
    scene::WaterComponent second;
    first.waveAmplitudeScale = 0.5f;
    second.waveAmplitudeScale = 3.0f;
    scene::ResolveWaterWaves(first, &material, {});
    scene::ResolveWaterWaves(second, &material, {});
    EXPECT_FLOAT_EQ(first.waves[0].amplitude, 0.5f);
    EXPECT_FLOAT_EQ(second.waves[0].amplitude, 3.0f);
    EXPECT_NEAR(second.current.x, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(second.current.y, 4.0f, testkit::kTolerance);
    second.enableGerstnerWaves = false;
    scene::ResolveWaterWaves(second, &material, {});
    EXPECT_FLOAT_EQ(second.waves[0].amplitude, 0.0f);
    EditMaterialAuthoring(material, [](auto& values) {
        values.params[scene::water_keys::kWaveAmplitude[0]] = {4.0f};
        values.params[scene::water_keys::kCurrentSpeed] = {10.0f};
    });
    scene::ResolveWaterWaves(first, &material, {});
    EXPECT_FLOAT_EQ(first.waves[0].amplitude, 2.0f);
    EXPECT_NEAR(first.current.x, 6.0f, testkit::kTolerance);
    EXPECT_FLOAT_EQ(retained->Data().params.at(scene::water_keys::kWaveAmplitude[0])[0], 1.0f);
}

/// @brief 材質の準備結果を追い出した後も、現在の設定を解決し直す。
TEST_F(WaterWavesAssetTest, MaterialPreparationEvictionAndMutableAuthoringKeepCurrentValues)
{
    std::array<asset::MaterialAsset, 40> materials;
    scene::WaterComponent water;
    for (size_t i = 0; i < materials.size(); ++i) {
        const float value = static_cast<float>(i + 1);
        EditMaterialAuthoring(materials[i], [value](auto& values) {
            values.params[scene::water_keys::kWaveAmplitude[0]] = {value};
            values.params[scene::water_keys::kFlowDirection] = {0.0f, 1.0f};
            values.params[scene::water_keys::kCurrentSpeed] = {value};
        });
        scene::ResolveWaterWaves(water, &materials[i], {});
        EXPECT_FLOAT_EQ(water.waves[0].amplitude, value);
        EXPECT_NEAR(water.current.y, value, testkit::kTolerance);
    }
    scene::ResolveWaterWaves(water, &materials.front(), {});
    EXPECT_FLOAT_EQ(water.waves[0].amplitude, 1.0f);
    asset::MaterialAssetAuthoring mutableValues;
    mutableValues.params[scene::water_keys::kWaveAmplitude[0]] = {7.0f};
    scene::ResolveWaterWaves(water, mutableValues, {});
    EXPECT_FLOAT_EQ(water.waves[0].amplitude, 7.0f);
    mutableValues.params[scene::water_keys::kWaveAmplitude[0]][0] = 9.0f;
    scene::ResolveWaterWaves(water, mutableValues, {});
    EXPECT_FLOAT_EQ(water.waves[0].amplitude, 9.0f);
}

/// @brief CPU の水面照会は位相オフセットと有効な波だけを使う。
TEST_F(WaterWavesAssetTest, CpuSurfaceQueryUsesThePhaseOffsetAndOnlyActiveWaves)
{
    scene::WaterComponent water;
    water.waveCount = 1;
    water.waveGrouping = 0.35f;
    water.waveSpread = 0.0f;
    water.waves[0] = {{1.0f, 0.0f}, 0.5f, 8.0f, 0.5f, 1.1f};
    water.waves[1] = {{-1.0f, 0.0f}, 3.0f, 0.8f, 0.7f, -0.2f};
    scene::Transform transform;
    constexpr float time = 1.25f;
    constexpr float targetPhase = 0.7f;
    const float k = math::TWO_PI / water.waves[0].wavelength;
    const float omega = std::sqrt(9.8f * k);
    const float baseX = (targetPhase + omega * time - water.waves[0].phaseOffsetRadians) / k;
    const float groupPhase = k * 0.09f * scene::WaterComponent::WAVE_GROUP_COS * baseX
        - 0.09f * omega * 0.5f * time + water.waves[0].phaseOffsetRadians;
    const float envelope = 1.0f + water.waveGrouping * std::sin(groupPhase);
    const float displayedX = baseX + water.waves[0].steepness * water.waves[0].amplitude
        * envelope * std::cos(targetPhase);
    scene::WaterSurfaceHeightSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(
        water, transform, {displayedX, -1.0f, 0.0f}, time, sample));
    EXPECT_NEAR(sample.displacement, water.waves[0].amplitude * envelope * std::sin(targetPhase), 0.001f);

    const float oneWaveHeight = water.GetSurfaceHeightAt(displayedX, 0.0f, time);
    water.waveCount = 2;
    const float twoWaveHeight = water.GetSurfaceHeightAt(displayedX, 0.0f, time);
    EXPECT_GT(std::abs(twoWaveHeight - oneWaveHeight), 0.01f);
    water.waveCount = 1;
    water.waves[1].direction = math::Vector2::ZERO;
    EXPECT_TRUE(scene::IsWaterSurfaceSupported(water, transform));
    water.waveCount = 2;
    ASSERT_FALSE(scene::IsWaterSurfaceSupported(water, transform));
}

/// @brief 同じ参照の解決に失敗した間は直前の正常波を使い、参照変更時は破棄する。
TEST_F(WaterWavesAssetTest, UpdateKeepsSameReferenceLastGoodAndClearsItWhenTheReferenceChanges)
{
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::WaterWavesAsset::TYPE_NAME));
    asset::WaterWavesAsset* source = Resolve();
    ASSERT_NE(source, nullptr);

    scene::Scene scene;
    physics::World world;
    scene::GameObject& object = scene.CreateGameObject("Water");
    auto& water = object.AddComponent<scene::WaterComponent>();
    water.wavesAssetRef.path = File();
    scene::WaterSystem system;

    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(water.waveCount, 8u);
    const auto lastGoodWaves = water.waves;

    source->waveCount = static_cast<int>(fluid::MAX_BASE_GERSTNER_WAVES + 1);
    UpdateWaterSystem(system, scene, world);
    EXPECT_EQ(water.waveCount, 8u);
    for (size_t i = 0; i < water.waveCount; ++i)
        EXPECT_TRUE(SameWave(water.waves[i], lastGoodWaves[i]));

    water.wavesAssetRef.path = m_temp.File("Assets/Missing.fzdata").generic_string();
    UpdateWaterSystem(system, scene, world);
    EXPECT_EQ(water.waveCount, 0u);
}

/// @brief プリセットと材質へ切り替えても別の波源の正常値を引き継がない。
TEST_F(WaterWavesAssetTest, UpdateKeepsPresetAndMaterialFallbackIndependent)
{
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::WaterWavesAsset::TYPE_NAME));
    auto* source = Resolve();
    ASSERT_NE(source, nullptr);

    scene::Scene scene;
    physics::World world;
    scene::GameObject& object = scene.CreateGameObject("Water");
    auto& water = object.AddComponent<scene::WaterComponent>();
    scene::WaterSystem system;

    UpdateWaterSystem(system, scene, world);
    const auto materialWaves = water.waves;
    const auto materialWaveCount = water.waveCount;
    ASSERT_GT(materialWaveCount, 0u);

    water.wavesAssetRef.path = File();
    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(water.waveCount, 8u);
    source->waveCount = static_cast<int>(fluid::MAX_BASE_GERSTNER_WAVES + 1);

    water.waveAuthoring.preset = scene::WaterSeaPreset::GENTLE;
    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(water.waveCount, 6u);

    water.waveAuthoring.preset = scene::WaterSeaPreset::AUTHORED;
    UpdateWaterSystem(system, scene, world);
    EXPECT_EQ(water.waveCount, 0u);

    water.wavesAssetRef.path.clear();
    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(water.waveCount, materialWaveCount);
    for (size_t index = 0; index < water.waveCount; ++index)
        EXPECT_TRUE(SameWave(water.waves[index], materialWaves[index]));
}

/// @brief 無効な間にキャッシュを退役させ、再有効化時に古い正常波を復活させない。
TEST_F(WaterWavesAssetTest, UpdatePrunesWaveHistoryWhileInactive)
{
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::WaterWavesAsset::TYPE_NAME));
    auto* source = Resolve();
    ASSERT_NE(source, nullptr);

    scene::Scene scene;
    physics::World world;
    scene::GameObject& object = scene.CreateGameObject("Water");
    auto& water = object.AddComponent<scene::WaterComponent>();
    water.wavesAssetRef.path = File();
    scene::WaterSystem system;

    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(water.waveCount, 8u);
    object.SetActive(false);
    source->waveCount = static_cast<int>(fluid::MAX_BASE_GERSTNER_WAVES + 1);
    UpdateWaterSystem(system, scene, world);

    object.SetActive(true);
    UpdateWaterSystem(system, scene, world);
    EXPECT_EQ(water.waveCount, 0u);
}

/// @brief 同一 EntityID へ再追加した波源に、削除したコンポーネントの正常波を引き継がない。
TEST_F(WaterWavesAssetTest, UpdateDoesNotRestoreRemovedComponentWaves)
{
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::WaterWavesAsset::TYPE_NAME));
    auto* source = Resolve();
    ASSERT_NE(source, nullptr);

    scene::Scene scene;
    physics::World world;
    scene::GameObject& object = scene.CreateGameObject("Water");
    auto& original = object.AddComponent<scene::WaterComponent>();
    original.wavesAssetRef.path = File();
    scene::WaterSystem system;
    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(original.waveCount, 8u);

    source->waveCount = static_cast<int>(fluid::MAX_BASE_GERSTNER_WAVES + 1);
    object.RemoveComponent<scene::WaterComponent>();
    auto& replacement = object.AddComponent<scene::WaterComponent>();
    replacement.wavesAssetRef.path = File();

    UpdateWaterSystem(system, scene, world);
    EXPECT_EQ(replacement.waveCount, 0u);
}

/// @brief 別シーンの同じ EntityID に正常波を引き継がない。
TEST_F(WaterWavesAssetTest, UpdateDoesNotCarryLastGoodWavesAcrossSceneGenerations)
{
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::WaterWavesAsset::TYPE_NAME));
    asset::WaterWavesAsset* source = Resolve();
    ASSERT_NE(source, nullptr);

    scene::Scene firstScene;
    scene::Scene secondScene;
    physics::World world;
    scene::GameObject& firstObject = firstScene.CreateGameObject("Water");
    scene::GameObject& secondObject = secondScene.CreateGameObject("Water");
    auto& firstWater = firstObject.AddComponent<scene::WaterComponent>();
    auto& secondWater = secondObject.AddComponent<scene::WaterComponent>();
    firstWater.wavesAssetRef.path = File();
    secondWater.wavesAssetRef.path = File();
    ASSERT_EQ(firstObject.GetID().index, secondObject.GetID().index);
    ASSERT_EQ(firstObject.GetID().generation, secondObject.GetID().generation);
    ASSERT_NE(firstScene.GetRenderSceneGeneration(), secondScene.GetRenderSceneGeneration());
    scene::WaterSystem system;

    UpdateWaterSystem(system, firstScene, world);
    ASSERT_EQ(firstWater.waveCount, 8u);
    source->waveCount = static_cast<int>(fluid::MAX_BASE_GERSTNER_WAVES + 1);
    UpdateWaterSystem(system, firstScene, world);
    ASSERT_EQ(firstWater.waveCount, 8u);

    UpdateWaterSystem(system, secondScene, world);
    EXPECT_EQ(secondWater.waveCount, 0u);
}

/// @brief 海況の向きを変えても同じ種の波形を保持し、独立した副うねりを加える。
TEST_F(WaterWavesAssetTest, SeaHeadingKeepsTheSeededWavePatternAndAddsIndependentCrossSwell)
{
    scene::Scene scene;
    physics::World world;
    auto& water = scene.CreateGameObject("Ocean").AddComponent<scene::WaterComponent>();
    water.waveAuthoring.preset = scene::WaterSeaPreset::OFFSHORE;
    scene::WaterSystem system;
    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(water.waveCount, 9u);
    const auto original = water.waves;

    water.waveAuthoring.overrideHeading = true;
    water.waveAuthoring.headingDegrees = 90.0f;
    water.waveAuthoring.secondaryHeadingDegrees = -90.0f;
    water.waveAuthoring.secondaryStrength = 0.3f;
    water.waveAuthoring.rippleMode = scene::WaterRippleDirectionMode::MANUAL;
    water.waveAuthoring.rippleHeadingDegrees = 180.0f;
    UpdateWaterSystem(system, scene, world);
    ASSERT_EQ(water.waveCount, 12u);
    for (size_t i = 0; i < 9; ++i) {
        EXPECT_NEAR(water.waves[i].direction.x, -original[i].direction.y, 1.0e-6f);
        EXPECT_NEAR(water.waves[i].direction.y, original[i].direction.x, 1.0e-6f);
        EXPECT_FLOAT_EQ(water.waves[i].phaseOffsetRadians, original[i].phaseOffsetRadians);
        EXPECT_FLOAT_EQ(water.waves[i].wavelength, original[i].wavelength);
        EXPECT_FLOAT_EQ(water.waves[i].amplitude, original[i].amplitude);
    }
    for (size_t i = 9; i < water.waveCount; ++i)
        EXPECT_LT(water.waves[i].direction.y, -0.95f);
    EXPECT_NEAR(water.rippleDirectionOverride.x, -1.0f, 1.0e-6f);
    EXPECT_NEAR(water.rippleDirectionOverride.y, 0.0f, 1.0e-6f);
    EXPECT_EQ(water.current, math::Vector2::ZERO);
}

/// @brief 旧形式に無い外観キーには、オフセットに依存せず既定値を使う。
TEST_F(WaterWavesAssetTest, ReflectedAppearanceKeepsLegacyMissingKeyDefaultsWithoutAssumingOffsets)
{
    renderer::ShaderDescriptor descriptor;
    descriptor.cbufferSize = sizeof(renderer::WaterEffectParams);
    const auto add = [&](const char* name, uint32_t offset, uint8_t columns) {
        renderer::ShaderVarDesc variable;
        variable.name = name;
        variable.offset = offset;
        variable.size = static_cast<uint32_t>(columns) * sizeof(float);
        variable.columns = columns;
        variable.varClass = columns == 1 ? renderer::ShaderVarClass::Scalar : renderer::ShaderVarClass::Vector;
        variable.varType = renderer::ShaderVarType::Float;
        descriptor.vars.push_back(variable);
    };
    add("rimGlowStrength", 0, 1);
    add("minShallowAlpha", 4, 1);
    add("specularStrength", 8, 1);
    add("skyReflectTint", 16, 3);
    add("rippleRingColor", 32, 3);
    add("rippleRingStrength", 44, 1);
    asset::MaterialAssetAuthoring values;
    std::vector<uint8_t> bytes;
    asset::InitializeWaterAppearanceParameters(values, descriptor, bytes);
    const renderer::WaterEffectParams legacy;
    ASSERT_EQ(bytes.size(), sizeof(legacy));
    EXPECT_EQ(std::memcmp(bytes.data(), &legacy, sizeof(legacy)), 0);

    descriptor.vars.clear();
    descriptor.cbufferSize = 80;
    add("rimGlowStrength", 64, 1);
    add("customBandWidth", 0, 1);
    values.params["rimGlowStrength"] = {0.15f};
    values.params["customBandWidth"] = {0.37f};
    asset::InitializeWaterAppearanceParameters(values, descriptor, bytes);
    float rim = 0, width = 0;
    std::memcpy(&rim, bytes.data() + 64, sizeof(rim));
    std::memcpy(&width, bytes.data(), sizeof(width));
    EXPECT_FLOAT_EQ(rim, 0.15f);
    EXPECT_FLOAT_EQ(width, 0.37f);
    EXPECT_EQ(bytes.size(), 80u);
}
} /// @note namespace fbzz::tests
