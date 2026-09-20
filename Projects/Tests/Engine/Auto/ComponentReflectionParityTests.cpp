/// @file    ComponentReflectionParityTests.cpp
/// @brief   Reflect() 経由へ畳んだコンポーネントが、旧シーンと同じキーを書き続けることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// SceneSerializer は 57 種類のコンポーネントを手書きで読み書きしていた。その大半は
/// 既に Reflect() を持っており、同じフィールド表が «保存側・読み込み側・Reflect()» の
/// 3 か所に分かれている状態だった。ParticleEmitter では実際に 44 フィールドがずれていた。
///
/// 畳むときの唯一にして最大の危険は **キー名の食い違い**。1 つでも違うと、
/// そのフィールドだけ «保存はされるのに開くと既定値» になる。エラーも警告も出ない。
///
/// ここに並ぶキー一覧は、**畳む前の手書きコードから機械的に抜き出したもの** ——
/// つまり «既に出荷されている .scene ファイルの中に実在するキー名» そのもの。
/// Reflect() でキーを改名すると、既存シーンのその項目が孤児になる。
/// このテストはそれを «改名した瞬間» に落とす。
///
/// @note 項目を **足す** ぶんには落ちない (kExpectedKeys に無いキーは «新規» として許す)。
///       落ちるのは «消えた / 名前が変わった» ときだけ。既存データを壊すのはそちらだけなので。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/ComponentReflectionCodec.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>

#include <Engine/Scene/Components/AtmosphericScatteringComponent.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshModifierComponent.hpp>
#include <Engine/Scene/Components/NavMeshSensorComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Components/PostProcessVolumeComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/Components/VolumetricCloudComponent.hpp>

#include <toml++/toml.hpp>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

std::set<std::string> KeysOf(const toml::table& table)
{
    std::set<std::string> keys;
    for (const auto& [key, value] : table) {
        (void)value;
        keys.insert(std::string(key));
    }
    return keys;
}

std::string Join(const std::set<std::string>& keys)
{
    std::string joined;
    for (const std::string& key : keys) {
        if (!joined.empty()) joined += ", ";
        joined += key;
    }
    return joined.empty() ? "(なし)" : joined;
}

} // namespace

class ComponentReflectionParityTest : public testkit::EngineFixture {
protected:
    /// component を 1 個だけ付けた GameObject をシーンとして保存し、そのテーブルを返す。
    /// 実際の保存経路を通すので、«畳んだ呼び出しを消してしまった» もここで落ちる。
    template<class T>
    toml::table SavedTable(const char* componentKey, const T& value)
    {
        scene::Scene scene;
        scene::GameObject& go = scene.CreateGameObject("Probe");
        go.AddComponent<T>(value);

        /// @note scenePath を空にすると副作用 (.terrain / .mat の書き出し) が起きない。
        const std::string text = scene::SceneSerializer::SaveToText(scene);
        EXPECT_FALSE(text.empty());

        toml::parse_result parsed = toml::parse(text);
        EXPECT_TRUE(parsed) << "シーン TOML が壊れています";
        if (!parsed) return {};

        const auto* objects = parsed.table()["gameobjects"].as_array();
        EXPECT_NE(objects, nullptr);
        if (objects == nullptr || objects->empty()) return {};

        const auto* first = (*objects)[0].as_table();
        EXPECT_NE(first, nullptr);
        if (first == nullptr) return {};

        const auto* componentTable = (*first)[componentKey].as_table();
        EXPECT_NE(componentTable, nullptr)
            << componentKey << " が保存されていません。"
            << "SceneSerializer から WriteComponentReflected の呼び出しが消えていませんか";
        return componentTable != nullptr ? *componentTable : toml::table{};
    }

    /// 出荷済みシーンに実在するキーが、今も全部書かれていることを確かめる。
    template<class T>
    void ExpectKeysStillWritten(const char* componentKey,
                                const std::vector<const char*>& expectedKeys,
                                const T& value = T{})
    {
        const std::set<std::string> actual = KeysOf(SavedTable<T>(componentKey, value));

        std::set<std::string> missing;
        for (const char* key : expectedKeys)
            if (actual.find(key) == actual.end()) missing.insert(key);

        EXPECT_TRUE(missing.empty())
            << componentKey << ": 旧シーンにあるキーが書かれなくなりました。"
            << "改名したなら、そのフィールドは既存シーンで既定値に戻ります → "
            << Join(missing);
    }
};

/// @name 畳み済みコンポーネント
/// キー一覧は畳む前の手書きコードから抜き出した「出荷済みシーンにあるキー名」。

TEST_F(ComponentReflectionParityTest, LightKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::LightComponent>("LightComponent",
        { "areaHeight", "areaTwoSided", "areaWidth", "castShadows", "color", "colorTemperature",
          "cookiePath", "cookieRotation", "enabled", "innerCone", "intensity", "outerCone",
          "range", "shadowBias", "shadowDistance", "shadowNearPlane", "shadowStrength",
          "sourceLength", "sourceRadius", "type", "useColorTemperature" });
}

TEST_F(ComponentReflectionParityTest, NavMeshAgentKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::NavMeshAgentComponent>("NavMeshAgentComponent",
        { "acceleration", "agentTypeId", "angularSpeedDeg", "areaMask", "autoBraking",
          "avoidancePriority", "enabled", "maxSpeed", "radius", "snapToNavMesh",
          "stoppingDistance", "updatePosition", "updateRotation" });
}

TEST_F(ComponentReflectionParityTest, NavMeshModifierKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::NavMeshModifierComponent>("NavMeshModifierComponent",
        { "areaType", "enabled", "mode" });
}

TEST_F(ComponentReflectionParityTest, NavMeshSensorKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::NavMeshSensorComponent>("NavMeshSensorComponent",
        { "autoChase", "chaseRepathInterval", "enabled", "heightThreshold", "memoryTime",
          "scanInterval", "targetTag", "useLineOfSight", "viewAngleDeg", "viewDistance" });
}

TEST_F(ComponentReflectionParityTest, FlowFieldWritesItsFlowList)
{
    /// @note 場は 2026-09-11 に «1 本» から «リスト» になり、2026-09-16 に FlowField へ改名した。
    ///       保存の外枠は forces 1 キーで、中身は ParticleEmitter::localForces と同じ形。
    scene::FlowField field{};
    scene::FlowFieldSettings uniform;
    uniform.fieldType = scene::FlowFieldType::Uniform;
    uniform.strength  = 3.0f;
    uniform.radius    = 8.0f;
    field.forces = { uniform };
    ExpectKeysStillWritten<scene::FlowField>("FlowField", { "forces" }, field);
}

TEST_F(ComponentReflectionParityTest, ReflectionProbeKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::ReflectionProbeComponent>("ReflectionProbeComponent",
        { "boxExtents", "boxInfluence", "captureMode", "captureResolution", "cubemapPath",
          "enabled", "influenceRadius", "intensity", "updateInterval" });
}

TEST_F(ComponentReflectionParityTest, SkyRendererKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::SkyRenderer>("SkyRenderer",
        { "atmosphereRadius", "cloudShadowCoverage", "cloudShadowSize", "cloudShadowSpeed",
          "cloudShadowStrength", "dayAltitude", "dayColor", "dayIntensity", "dayNightEnabled",
          "enabled", "mieG", "mieScattering", "nightAltitude", "nightColor", "nightIntensity",
          "planetRadius", "rayleighScattering", "skyDayBrightness", "skyNightBrightness",
          "skyScatterIntensity", "skySunsetBrightness", "sunsetColor", "sunsetIntensity" });
}

TEST_F(ComponentReflectionParityTest, SunMoonRendererKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::SunMoonRenderer>("SunMoonRenderer",
        { "enabled", "moonBrightness", "moonColor", "moonEnabled", "moonSize",
          "sunDiskIntensity", "sunEnabled" });
}

TEST_F(ComponentReflectionParityTest, TrailKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::TrailComponent>("TrailComponent",
        { "alignment", "attachBone", "attachOffset", "beamEnd", "beamMode", "beamStart",
          "clearOnDisable", "colorEnd", "colorStart", "duration", "enabled", "materialPath",
          "maxPoints", "minVertexDist", "sampleInterval", "smoothSubdivisions", "uvMode",
          "uvScrollSpeed", "uvTiling", "widthEasing", "widthEnd", "widthStart" });
}

TEST_F(ComponentReflectionParityTest, VolumeKeepsItsSceneKeys)
{
    /// @note buoyancy / drag は «意図して消した» キー。前者は VolumeType::Buoyancy ごと
    ///       WaterComponent へ移り (buoyancy.md)、後者はどの VolumeType も読んでいない
    ///       死んだノブだった (flow-field.md)。消したキーはここからも外す ── 残すと
    ///       «改名を捕まえる網» が «廃止を禁じる鎖» になる。
    ExpectKeysStillWritten<scene::VolumeComponent>("VolumeComponent",
        { "duration", "elapsed", "enabled", "explosionImpulse", "gravity",
          "inwardStrength", "liftStrength", "magneticField", "swirlStrength", "timeScale",
          "type" });
}

TEST_F(ComponentReflectionParityTest, VolumetricCloudKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::VolumetricCloudComponent>("VolumetricCloudComponent",
        { "albedo", "ambientGradient", "ambientStrength", "ambientTint", "anisotropy",
          "bottomHeight", "bottomSoftness", "cloudSize", "coverage", "density", "detailSize",
          "detailStrength", "enabled", "evolutionSpeed", "extinction", "fadeDistance",
          "halfResolution", "horizonFade", "lightAbsorption", "lightShaftStrength",
          "lightStepCount", "maxDistance", "minDistance", "multiScatter", "powderStrength",
          "silverLining", "stepCount", "sunIntensity", "sunTint", "thickness", "topSoftness",
          "weatherAmount", "weatherSize", "windDirection", "windSpeed" });
}

TEST_F(ComponentReflectionParityTest, AtmosphericScatteringKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::AtmosphericScatteringComponent>("AtmosphericScatteringComponent",
        { "enabled", "fogColor", "fogDensity", "fogEnabled", "fogFar", "fogSource" });
}

TEST_F(ComponentReflectionParityTest, EnvironmentLightKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::EnvironmentLightComponent>("EnvironmentLightComponent",
        { "diffuseScale", "enabled", "intensity", "irradiancePath", "maxMipLevel",
          "prefilterPath", "source", "specularScale" });
}

TEST_F(ComponentReflectionParityTest, PostProcessVolumeKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::PostProcessVolumeComponent>("PostProcessVolumeComponent",
        { "blendDistance", "blendWeight", "enabled", "influenceRadius", "isGlobal", "priority",
          "profile" });
}

TEST_F(ComponentReflectionParityTest, RagdollKeepsItsSceneKeys)
{
    ExpectKeysStillWritten<scene::RagdollComponent>("RagdollComponent",
        { "activateOnStart", "angularDrag", "blendIn", "blendOut", "collapseDistance",
          "contactDynamic", "contactSelf", "contactWhileActive", "contactWorld", "driveDamping",
          "driveFalloff", "driveScale", "enabled", "friction", "gravity", "groundOffset",
          "groundPlane", "impactSlack", "learnLimits", "limitMargin", "linearDrag", "maxDepth",
          "profile", "recoverySeconds", "restitution", "rootAnchor", "rootAnchorSag",
          "rootAnchorTilt", "rootBoneName", "selfSkip", "simulateInEditor", "substeps" });
}

TEST_F(ComponentReflectionParityTest, NavMeshOffMeshLinkKeepsItsSceneKeys)
{
    /// @note enabled は保存されていたのに Reflect に無く、AI バスから見えなかった。
    ///       畳むにあたって Reflect へ足したので、ここで «消えていない» ことを押さえる。
    ExpectKeysStillWritten<scene::NavMeshOffMeshLinkComponent>("NavMeshOffMeshLinkComponent",
        { "activated", "agentTypeMask", "bidirectional", "enabled", "endPoint", "startPoint",
          "traversalTime" });
}

TEST_F(ComponentReflectionParityTest, MeshTrailKeepsItsSceneKeys)
{
    /// @note excludedMeshIndices も同じ («保存はされるが Reflect に無い» 側)。
    ExpectKeysStillWritten<scene::MeshTrailComponent>("MeshTrailComponent",
        { "clearOnDisable", "colorEnd", "colorStart", "doubleSided", "duration", "enabled",
          "excludedMeshIndices", "materialPath", "maxSamples", "minVertexDist",
          "sampleInterval" });
}

/// @name 往復
/// キーが揃っていても «値が落ちない» は別の話。畳んだ経路が実際に値を運ぶことを
/// 通しで確かめる (キー名は上で全件見ているので、ここは代表 1 つでよい)。

TEST_F(ComponentReflectionParityTest, AFoldedComponentKeepsItsValues)
{
    scene::Scene source;
    scene::GameObject& go = source.CreateGameObject("Probe");

    scene::ReflectionProbeComponent probe{};
    probe.enabled         = false;
    probe.intensity       = 2.25f;
    probe.influenceRadius = 17.5f;
    probe.cubemapPath     = "Assets/Ibl/Room.dds";
    go.AddComponent<scene::ReflectionProbeComponent>(probe);

    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* restoredObject = nullptr;
    for (auto& candidate : loaded->GameObjects())
        if (candidate.name == "Probe") restoredObject = &candidate;
    ASSERT_NE(restoredObject, nullptr);

    const auto* restored = restoredObject->GetComponent<scene::ReflectionProbeComponent>();
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->enabled);
    EXPECT_NEAR(restored->intensity, 2.25f, testkit::kTolerance);
    EXPECT_NEAR(restored->influenceRadius, 17.5f, testkit::kTolerance);
    EXPECT_EQ(restored->cubemapPath, "Assets/Ibl/Room.dds");
}

/// @note Light Probe Volume は設定だけを運ぶ。焼いた結果と焼きの要求はランタイム値で、
///       読み直したシーンは «未ベイク» から自動で焼き直す (古い GPU ハンドルを復元しない)。
TEST_F(ComponentReflectionParityTest, LightProbeVolumeKeepsSettingsButNotBakeState)
{
    scene::Scene source;
    scene::GameObject& go = source.CreateGameObject("GI");

    scene::LightProbeVolumeComponent volume{};
    volume.boxExtents     = { 6.0f, 3.0f, 4.0f };
    volume.probeCountX    = 12;
    volume.probeCountY    = 5;
    volume.probeCountZ    = 9;
    volume.intensity      = 1.5f;
    volume.bounces        = 3;
    volume.realtimeUpdate = true;
    volume.bakeRequested  = true;
    volume.runtimeReady   = true;
    go.AddComponent<scene::LightProbeVolumeComponent>(volume);

    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.find("bakeRequested"), std::string::npos);
    EXPECT_EQ(text.find("runtime"), std::string::npos);

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);
    const scene::LightProbeVolumeComponent* restored = nullptr;
    for (auto& candidate : loaded->GameObjects())
        if (candidate.name == "GI") restored = candidate.GetComponent<scene::LightProbeVolumeComponent>();
    ASSERT_NE(restored, nullptr);
    EXPECT_VEC3_NEAR(restored->boxExtents, math::Vector3(6.0f, 3.0f, 4.0f), testkit::kTolerance);
    EXPECT_EQ(restored->probeCountX, 12);
    EXPECT_EQ(restored->probeCountY, 5);
    EXPECT_EQ(restored->probeCountZ, 9);
    EXPECT_NEAR(restored->intensity, 1.5f, testkit::kTolerance);
    EXPECT_EQ(restored->bounces, 3);
    EXPECT_TRUE(restored->realtimeUpdate);
    EXPECT_FALSE(restored->bakeRequested);
    EXPECT_FALSE(restored->runtimeReady);
}

TEST_F(ComponentReflectionParityTest, LightProbeVolumeClampsItsGrid)
{
    scene::LightProbeVolumeComponent volume{};
    volume.probeCountX = 0;
    volume.probeCountY = 500;
    volume.probeCountZ = -3;
    const auto grid = volume.ClampedGrid();
    EXPECT_EQ(grid[0], 1);
    EXPECT_EQ(grid[1], 32);
    EXPECT_EQ(grid[2], 1);
    EXPECT_EQ(volume.ProbeCount(), 32);
}

/// @name 廃止コンポーネントの移行

TEST_F(ComponentReflectionParityTest, LegacyWindZoneBecomesAmbientWindForces)
{
    /// @note 旧 WindZoneComponent を持つシーンを «手で組んだ TOML» として読ませる。
    ///       捨てると既存シーンの風が黙って止まる (しかも «風が弱い» と区別が付かない)。
    const std::string legacy = R"(
[[gameobjects]]
name = "Wind Zone"
instanceId = 1
[gameobjects.WindZoneComponent]
enabled = true
direction = [0.0, 0.0, 1.0]
strength = 4.0
turbulence = 2.5
pulseFrequency = 3.0
)";

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(legacy, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* object = nullptr;
    for (auto& candidate : loaded->GameObjects())
        if (candidate.name == "Wind Zone") object = &candidate;
    ASSERT_NE(object, nullptr);

    /// @note 環境風は GameObject ではなくシーン設定へ移る。«どれが環境風か» が並び順で
    ///       決まる状態をやめるのがこの移行の目的なので、場としては残さない。
    EXPECT_EQ(object->GetComponent<scene::FlowField>(), nullptr);

    const scene::SceneEnvironment& environment = loaded->Environment();
    EXPECT_TRUE(environment.enabled);
    EXPECT_VEC3_NEAR(environment.direction, math::Vector3(0.0f, 0.0f, 1.0f), testkit::kTolerance);
    /// @note 旧 strength は加速度 [m/s^2]。静止粒子で dv を等置して流速 [m/s] へ写す。
    EXPECT_NEAR(environment.speed, 4.0f * scene::kLegacyAccelerationToFlowSpeed,
                testkit::kTolerance);
    EXPECT_NEAR(environment.turbulence, 2.5f * scene::kLegacyAccelerationToFlowSpeed,
                testkit::kTolerance);
    /// @note 旧 pulseFrequency は «脈動の速さ» で、乱流の時間スクロール速度と同じ意味。
    EXPECT_NEAR(environment.pulseFrequency, 3.0f, testkit::kTolerance);
}

TEST_F(ComponentReflectionParityTest, TheLegacyForceFieldKeyIsStillRead)
{
    /// @note 型は ParticleForceField → ForceField → FlowField と変わり、そのたび保存キーも変わった。
    ///       出荷済みシーンは旧キーで書かれている。読めなくなると場が丸ごと消える。
    const std::string legacy = R"(
[[gameobjects]]
name = "Old Field"
instanceId = 1
[gameobjects.ParticleForceField]
forces = [ { fieldType = 1, strength = 9.0, radius = 3.0 } ]
)";

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(legacy, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* object = nullptr;
    for (auto& candidate : loaded->GameObjects())
        if (candidate.name == "Old Field") object = &candidate;
    ASSERT_NE(object, nullptr);

    const auto* field = object->GetComponent<scene::FlowField>();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->forces.size(), 1u);
    /// @note fieldType の整数値は旧 enum と揃えて固定してある (1 = Attract → Sink)。
    EXPECT_EQ(field->forces[0].fieldType, scene::FlowFieldType::Sink);
    EXPECT_NEAR(field->forces[0].strength, 9.0f * scene::kLegacyAccelerationToFlowSpeed,
                testkit::kTolerance);
}

TEST_F(ComponentReflectionParityTest, LegacyFlatForceFieldBecomesASingleFlow)
{
    /// @note 場が «1 本» だった頃 (〜2026-09-11) の形。キーが直下にフラットに並ぶ。
    const std::string legacy = R"(
[[gameobjects]]
name = "Vortex"
instanceId = 1
[gameobjects.ForceField]
enabled = true
fieldType = 3
strength = 12.0
radius = 6.0
direction = [0.0, 1.0, 0.0]
)";

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(legacy, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* object = nullptr;
    for (auto& candidate : loaded->GameObjects())
        if (candidate.name == "Vortex") object = &candidate;
    ASSERT_NE(object, nullptr);

    const auto* field = object->GetComponent<scene::FlowField>();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->forces.size(), 1u);
    EXPECT_EQ(field->forces[0].fieldType, scene::FlowFieldType::Vortex);
    EXPECT_NEAR(field->forces[0].strength, 12.0f * scene::kLegacyAccelerationToFlowSpeed,
                testkit::kTolerance);
    EXPECT_NEAR(field->forces[0].radius, 6.0f, testkit::kTolerance);
}

TEST_F(ComponentReflectionParityTest, LegacyGlobalWindBecomesTheSceneEnvironment)
{
    /// @note [environment] を持たないシーンでは «半径 0 の Uniform (+ Curl)» が環境風だった。
    ///       残したままにすると環境流と局所の場で二重に掛かる。
    const std::string legacy = R"(
[[gameobjects]]
name = "Air"
instanceId = 1
[gameobjects.ForceField]
forces = [ { fieldType = 0, strength = 5.0, radius = 0.0, direction = [1.0, 0.0, 0.0] },
           { fieldType = 4, strength = 2.0, radius = 0.0, noiseSpeed = 1.5 } ]
)";

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(legacy, "");
    ASSERT_NE(loaded, nullptr);

    const scene::SceneEnvironment& environment = loaded->Environment();
    EXPECT_TRUE(environment.enabled);
    EXPECT_VEC3_NEAR(environment.direction, math::Vector3(1.0f, 0.0f, 0.0f), testkit::kTolerance);
    EXPECT_NEAR(environment.speed, 5.0f * scene::kLegacyAccelerationToFlowSpeed,
                testkit::kTolerance);
    EXPECT_NEAR(environment.turbulence, 2.0f * scene::kLegacyAccelerationToFlowSpeed,
                testkit::kTolerance);
    EXPECT_NEAR(environment.pulseFrequency, 1.5f, testkit::kTolerance);

    for (auto& candidate : loaded->GameObjects())
        EXPECT_EQ(candidate.GetComponent<scene::FlowField>(), nullptr)
            << "環境流へ写した 2 本を残すと二重に掛かる";
}

TEST_F(ComponentReflectionParityTest, TheSceneEnvironmentSurvivesTheRoundTrip)
{
    scene::Scene scene;
    scene::SceneEnvironment& environment = scene.Environment();
    environment.enabled        = true;
    environment.direction      = { 0.0f, 0.0f, -1.0f };
    environment.speed          = 7.5f;
    environment.turbulence     = 1.25f;
    environment.pulseFrequency = 2.5f;

    const std::string text = scene::SceneSerializer::SaveToText(scene);
    ASSERT_FALSE(text.empty());

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);

    const scene::SceneEnvironment& restored = loaded->Environment();
    EXPECT_TRUE(restored.enabled);
    EXPECT_VEC3_NEAR(restored.direction, math::Vector3(0.0f, 0.0f, -1.0f), testkit::kTolerance);
    EXPECT_NEAR(restored.speed, 7.5f, testkit::kTolerance);
    EXPECT_NEAR(restored.turbulence, 1.25f, testkit::kTolerance);
    EXPECT_NEAR(restored.pulseFrequency, 2.5f, testkit::kTolerance);
}

/// キー名が合っていても «値の書き方» が変わると同じように黙って壊れる。
/// LightComponent::type は手書き時代だけ文字列で、Reflect() は他の enum と同じ int を読む。
/// 移行が抜けると Spot も Tube も既定値の Directional に落ち、次の保存で書き戻されて消える。
TEST_F(ComponentReflectionParityTest, LegacyStringLightTypeSurvivesLoad)
{
    const std::string legacy = R"(
[[gameobjects]]
name = "TubeLamp"
instanceId = 1
[gameobjects.LightComponent]
type = 'Tube'
enabled = true
intensity = 4.0
range = 7.0
sourceLength = 2.5
)";

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(legacy, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* object = nullptr;
    for (auto& candidate : loaded->GameObjects())
        if (candidate.name == "TubeLamp") object = &candidate;
    ASSERT_NE(object, nullptr);

    const auto* light = object->GetComponent<scene::LightComponent>();
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->type, scene::LightComponent::Type::Tube);
    EXPECT_NEAR(light->intensity, 4.0f, testkit::kTolerance);
    EXPECT_NEAR(light->sourceLength, 2.5f, testkit::kTolerance);
}

TEST_F(ComponentReflectionParityTest, IntLightTypeStillLoads)
{
    const std::string current = R"(
[[gameobjects]]
name = "SpotLamp"
instanceId = 1
[gameobjects.LightComponent]
type = 2
enabled = true
outerCone = 45.0
)";

    std::unique_ptr<scene::Scene> loaded = scene::SceneSerializer::LoadDataFromText(current, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* object = nullptr;
    for (auto& candidate : loaded->GameObjects())
        if (candidate.name == "SpotLamp") object = &candidate;
    ASSERT_NE(object, nullptr);

    const auto* light = object->GetComponent<scene::LightComponent>();
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->type, scene::LightComponent::Type::Spot);
    EXPECT_NEAR(light->outerCone, 45.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
