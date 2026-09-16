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
#include <Engine/Scene/Components/ForceField.hpp>
#include <Engine/Scene/Components/PostProcessVolumeComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/ReflectionProbeComponent.hpp>
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

        // scenePath を空にすると副作用 (.terrain / .mat の書き出し) が起きない。
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

// ── 畳み済みコンポーネント ──────────────────────────────────────────────────
// キー一覧は畳む前の手書きコードから抜き出した「出荷済みシーンにあるキー名」。

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

TEST_F(ComponentReflectionParityTest, ForceFieldWritesItsForceList)
{
    // 力場は 2026-09-11 に «1 本» から «リスト» になった (旧 WindZone を吸収するため)。
    // 保存の外枠は forces 1 キーで、中身は ParticleEmitter::localForces と同じ形。
    scene::ForceField field{};
    field.forces = scene::MakeAmbientWindForces({ 1.0f, 0.0f, 0.0f }, 3.0f, 1.0f, 2.0f);
    ExpectKeysStillWritten<scene::ForceField>("ForceField", { "forces" }, field);
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
    ExpectKeysStillWritten<scene::VolumeComponent>("VolumeComponent",
        { "buoyancy", "drag", "duration", "elapsed", "enabled", "explosionImpulse", "gravity",
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
    // enabled は保存されていたのに Reflect に無く、AI バスから見えなかった。
    // 畳むにあたって Reflect へ足したので、ここで «消えていない» ことを押さえる。
    ExpectKeysStillWritten<scene::NavMeshOffMeshLinkComponent>("NavMeshOffMeshLinkComponent",
        { "activated", "agentTypeMask", "bidirectional", "enabled", "endPoint", "startPoint",
          "traversalTime" });
}

TEST_F(ComponentReflectionParityTest, MeshTrailKeepsItsSceneKeys)
{
    // excludedMeshIndices も同じ («保存はされるが Reflect に無い» 側)。
    ExpectKeysStillWritten<scene::MeshTrailComponent>("MeshTrailComponent",
        { "clearOnDisable", "colorEnd", "colorStart", "doubleSided", "duration", "enabled",
          "excludedMeshIndices", "materialPath", "maxSamples", "minVertexDist",
          "sampleInterval" });
}

// ── 往復 ────────────────────────────────────────────────────────────────────
// キーが揃っていても «値が落ちない» は別の話。畳んだ経路が実際に値を運ぶことを
// 通しで確かめる (キー名は上で全件見ているので、ここは代表 1 つでよい)。

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

// ── 廃止コンポーネントの移行 ────────────────────────────────────────────────

TEST_F(ComponentReflectionParityTest, LegacyWindZoneBecomesAmbientWindForces)
{
    // 旧 WindZoneComponent を持つシーンを «手で組んだ TOML» として読ませる。
    // 捨てると既存シーンの風が黙って止まる (しかも «風が弱い» と区別が付かない)。
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

    const auto* field = object->GetComponent<scene::ForceField>();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->forces.size(), 2u) << "風 (Wind) と乱れ (Turbulence) の 2 本になるはず";

    const auto& wind = field->forces[0];
    EXPECT_EQ(wind.fieldType, scene::ForceFieldType::Wind);
    EXPECT_VEC3_NEAR(wind.direction, math::Vector3(0.0f, 0.0f, 1.0f), testkit::kTolerance);
    EXPECT_NEAR(wind.strength, 4.0f, testkit::kTolerance);
    // radius 0 = 減衰なしでシーン全体。ここが 5 だと «風が 5m で止まる» になる。
    EXPECT_NEAR(wind.radius, 0.0f, testkit::kTolerance);

    const auto& turbulence = field->forces[1];
    EXPECT_EQ(turbulence.fieldType, scene::ForceFieldType::Turbulence);
    EXPECT_NEAR(turbulence.strength, 2.5f, testkit::kTolerance);
    // 旧 pulseFrequency は «脈動の速さ» で、乱流の時間スクロール速度と同じ意味。
    EXPECT_NEAR(turbulence.noiseSpeed, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(turbulence.radius, 0.0f, testkit::kTolerance);
}

TEST_F(ComponentReflectionParityTest, TheLegacyForceFieldKeyIsStillRead)
{
    // 型が ParticleForceField → ForceField になったので保存キーも変わった。
    // 出荷済みシーンは旧キーで書かれている。読めなくなると力場が丸ごと消える。
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

    const auto* field = object->GetComponent<scene::ForceField>();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->forces.size(), 1u);
    EXPECT_EQ(field->forces[0].fieldType, scene::ForceFieldType::Attract);
    EXPECT_NEAR(field->forces[0].strength, 9.0f, testkit::kTolerance);
}

TEST_F(ComponentReflectionParityTest, LegacyFlatForceFieldBecomesASingleForce)
{
    // 力場が «1 本» だった頃 (〜2026-09-11) の形。キーが直下にフラットに並ぶ。
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

    const auto* field = object->GetComponent<scene::ForceField>();
    ASSERT_NE(field, nullptr);
    ASSERT_EQ(field->forces.size(), 1u);
    EXPECT_EQ(field->forces[0].fieldType, scene::ForceFieldType::Vortex);
    EXPECT_NEAR(field->forces[0].strength, 12.0f, testkit::kTolerance);
    EXPECT_NEAR(field->forces[0].radius, 6.0f, testkit::kTolerance);
}

// キー名が合っていても «値の書き方» が変わると同じように黙って壊れる。
// LightComponent::type は手書き時代だけ文字列で、Reflect() は他の enum と同じ int を読む。
// 移行が抜けると Spot も Tube も既定値の Directional に落ち、次の保存で書き戻されて消える。
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
