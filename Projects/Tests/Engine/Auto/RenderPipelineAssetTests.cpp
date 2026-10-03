/// @file    RenderPipelineAssetTests.cpp
/// @brief   Typed rendering asset persistence, fallback and migration contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

namespace fbzz::tests {
namespace {

/// @note Exercise the same structural requests as ImGuiReflector without a window or GPU.
class PassOverrideEditor final : public scene::IReflector {
public:
    size_t additionalCount = 0;
    size_t removeIndex = NO_REMOVE;
    size_t moveFrom = 0;
    size_t moveTo = 0;
    size_t moveQueries = 0;
    size_t renamedIndex = NO_REMOVE;
    std::string renamedName;
    std::string newEntryName = "AddedPass";
    bool requestMove = false;

    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char* name, std::string& value) override
    {
        if (std::string_view(name) != "name") return;
        if (m_index == renamedIndex) value = renamedName;
        else if (value.empty()) value = newEntryName;
    }
    void BeginObjectElement(size_t index) override { m_index = index; }
    size_t BeginObjectList(const char* name, size_t count) override
    {
        EXPECT_STREQ(name, "passOverrides");
        return count + additionalCount;
    }
    size_t EndObjectList() override { return removeIndex; }
    bool ObjectListMove(size_t& from, size_t& to) override
    {
        ++moveQueries;
        from = moveFrom;
        to = moveTo;
        return requestMove;
    }
private:
    size_t m_index = NO_REMOVE;
};

class RenderPipelineAssetTest : public testkit::EngineFixture {
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
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }
    std::string File(const char* name = "Pipeline.fzdata") const
    {
        return m_temp.File(std::string("Assets/") + name).generic_string();
    }
    void Write(const std::string& path, const std::string& text) const
    {
        std::ofstream stream(path, std::ios::binary);
        ASSERT_TRUE(stream.good());
        stream << text;
    }
    std::string Text(const std::string& path) const
    {
        std::string text;
        EXPECT_TRUE(util::FileSystem::ReadText(path, text));
        return text;
    }
    asset::RenderPipelineAsset* Resolve(const std::string& path) const
    {
        auto* data = asset::DataAssetRegistry::Resolve(path);
        if (!data || std::string(data->GetTypeName()) != asset::RenderPipelineAsset::TYPE_NAME) return nullptr;
        return static_cast<asset::RenderPipelineAsset*>(data);
    }
    renderer::RenderSettings Distinct() const
    {
        renderer::RenderSettings value;
        value.pipeline = renderer::RenderingPipeline::DeferredPlus;
        value.modeRequest = {renderer::RenderMode::HYBRID, renderer::PathTracingProfile::GAME, true, false, true};
        value.hybridQuality = renderer::MakeHybridQualityPreset(renderer::HybridQualityPreset::BALANCED);
        value.hybridQuality.maxHistoryMiB = 320;
        value.hybridQuality.maxProbeCapturesPerFrame = 2;
        value.hybridQuality.asUpdateBudgetMs = 1.25f;
        value.hybridQuality.reconstructionBudgetMs = 1.5f;
        value.hybridQuality.probeUpdateBudgetMs = 0.75f;
        value.shadowEnabled = false;
        value.shadow.mapResolution = 4096;
        value.shadow.cascadeCount = 3;
        value.shadow.cascadeSplitLambda = 0.625f;
        value.shadow.cascadeBlend = 0.25f;
        value.shadow.autoFitDistance = 256;
        value.shadow.pcfRadius = 3;
        value.shadow.pcssEnabled = true;
        value.shadow.pcssLightRadius = 4;
        value.shadow.punctualMapResolution = 1024;
        value.shadow.punctualPcfRadius = 2;
        value.shadow.maxShadowedPointLights = 1;
        value.shadow.punctualShadowDistance = 128;
        value.gpuInstancing = false;
        value.asyncCompute = true;
        value.clustered.enabled = false;
        value.clustered.maxDistance = 512;
        value.particleBudgetEnabled = false;
        value.particleBudget = -1;
        value.froxelFog.gridX = 32; value.froxelFog.gridY = 24; value.froxelFog.gridZ = 16;
        value.schedulePolicy = renderer::RenderGraphSchedulePolicy::MinimizeLifetimes;
        value.passOverrides = {{"DeferredLighting", false, false, {"SceneDepth", "GBuffer0"}}};
        return value;
    }
    void ExpectOwned(const renderer::RenderSettings& actual, const renderer::RenderSettings& expected) const
    {
        EXPECT_EQ(actual.pipeline, expected.pipeline);
        EXPECT_EQ(actual.modeRequest.mode, expected.modeRequest.mode);
        EXPECT_EQ(actual.modeRequest.pathProfile, expected.modeRequest.pathProfile);
        EXPECT_EQ(actual.modeRequest.rayShadow, expected.modeRequest.rayShadow);
        EXPECT_EQ(actual.modeRequest.rayReflection, expected.modeRequest.rayReflection);
        EXPECT_EQ(actual.modeRequest.rayDiffuseGi, expected.modeRequest.rayDiffuseGi);
        EXPECT_EQ(actual.hybridQuality, expected.hybridQuality);
        EXPECT_EQ(actual.shadowEnabled, expected.shadowEnabled);
        EXPECT_EQ(actual.shadow.mapResolution, expected.shadow.mapResolution);
        EXPECT_EQ(actual.shadow.cascadeCount, expected.shadow.cascadeCount);
        EXPECT_FLOAT_EQ(actual.shadow.cascadeSplitLambda, expected.shadow.cascadeSplitLambda);
        EXPECT_FLOAT_EQ(actual.shadow.cascadeBlend, expected.shadow.cascadeBlend);
        EXPECT_FLOAT_EQ(actual.shadow.autoFitDistance, expected.shadow.autoFitDistance);
        EXPECT_EQ(actual.shadow.pcfRadius, expected.shadow.pcfRadius);
        EXPECT_EQ(actual.shadow.pcssEnabled, expected.shadow.pcssEnabled);
        EXPECT_FLOAT_EQ(actual.shadow.pcssLightRadius, expected.shadow.pcssLightRadius);
        EXPECT_EQ(actual.shadow.punctualMapResolution, expected.shadow.punctualMapResolution);
        EXPECT_EQ(actual.shadow.punctualPcfRadius, expected.shadow.punctualPcfRadius);
        EXPECT_EQ(actual.shadow.maxShadowedPointLights, expected.shadow.maxShadowedPointLights);
        EXPECT_FLOAT_EQ(actual.shadow.punctualShadowDistance, expected.shadow.punctualShadowDistance);
        EXPECT_EQ(actual.gpuInstancing, expected.gpuInstancing);
        EXPECT_EQ(actual.asyncCompute, expected.asyncCompute);
        EXPECT_EQ(actual.clustered.enabled, expected.clustered.enabled);
        EXPECT_FLOAT_EQ(actual.clustered.maxDistance, expected.clustered.maxDistance);
        EXPECT_EQ(actual.particleBudget, expected.particleBudget);
        EXPECT_EQ(actual.particleBudgetEnabled, expected.particleBudgetEnabled);
        EXPECT_EQ(actual.froxelFog.gridX, expected.froxelFog.gridX);
        EXPECT_EQ(actual.froxelFog.gridY, expected.froxelFog.gridY);
        EXPECT_EQ(actual.froxelFog.gridZ, expected.froxelFog.gridZ);
        EXPECT_EQ(actual.schedulePolicy, expected.schedulePolicy);
        ASSERT_EQ(actual.passOverrides.size(), expected.passOverrides.size());
        for (size_t i = 0; i < actual.passOverrides.size(); ++i) {
            EXPECT_EQ(actual.passOverrides[i].name, expected.passOverrides[i].name);
            EXPECT_EQ(actual.passOverrides[i].enabled, expected.passOverrides[i].enabled);
            EXPECT_EQ(actual.passOverrides[i].allowCulling, expected.passOverrides[i].allowCulling);
            EXPECT_EQ(actual.passOverrides[i].extraReads, expected.passOverrides[i].extraReads);
        }
    }
    void ExpectPassUnknownFields(std::initializer_list<const char*> values) const
    {
        auto parsed = toml::parse(Text(File()));
        ASSERT_TRUE(parsed);
        const auto* entries = parsed.table()["graph"]["passOverrides"].as_array();
        ASSERT_NE(entries, nullptr);
        ASSERT_EQ(entries->size(), values.size());
        size_t index = 0;
        for (const char* expected : values) {
            const auto* entry = (*entries)[index++].as_table();
            ASSERT_NE(entry, nullptr);
            if (expected) EXPECT_EQ((*entry)["future"].value<std::string>(), std::string(expected));
            else EXPECT_FALSE(entry->contains("future"));
        }
    }
    testkit::TempDir m_temp{"RenderPipelineAsset"};
};

TEST_F(RenderPipelineAssetTest, CapturesPersistentFieldsWithoutOverwritingTransientOrVolumeSettings)
{
    auto source = Distinct();
    source.viewMode = renderer::ViewMode::WireframeLit;
    source.shadow.debugVisualizeCascades = true;
    source.clustered.debugHeatmap = true;
    source.clustered.forceAllLights = true;
    source.userBrightness = 2;
    source.ssr.enabled = true;
    asset::RenderPipelineAsset pipeline;
    ASSERT_TRUE(pipeline.Capture(source));
    renderer::RenderSettings target;
    target.userBrightness = 0.5f; target.renderScale = 0.75f; target.userBloomScale = 0.25f;
    target.ibl.intensity = 9; target.postProcess.exposure = 3;
    target.taa.enabled = true; target.froxelFog.density = 0.125f;
    target.clustered.forceAllLights = true; target.shadow.debugVisualizeCascades = true;
    target.selectedObjects = {{3, 7}};
    pipeline.ApplyTo(target);
    ExpectOwned(target, source);
    EXPECT_EQ(target.viewMode, renderer::ViewMode::Lit);
    EXPECT_FALSE(target.clustered.debugHeatmap);
    EXPECT_TRUE(target.clustered.forceAllLights);
    EXPECT_TRUE(target.shadow.debugVisualizeCascades);
    EXPECT_FALSE(pipeline.Settings().shadow.debugVisualizeCascades);
    EXPECT_FALSE(pipeline.Settings().clustered.forceAllLights);
    EXPECT_FLOAT_EQ(target.userBrightness, 0.5f);
    EXPECT_FLOAT_EQ(target.renderScale, 0.75f);
    EXPECT_FLOAT_EQ(target.userBloomScale, 0.25f);
    EXPECT_FLOAT_EQ(target.ibl.intensity, 9);
    EXPECT_FLOAT_EQ(target.postProcess.exposure, 3);
    EXPECT_TRUE(target.taa.enabled);
    EXPECT_FALSE(target.ssr.enabled);
    EXPECT_FLOAT_EQ(target.froxelFog.density, 0.125f);
    ASSERT_EQ(target.selectedObjects.size(), 1u);
    EXPECT_EQ(target.selectedObjects[0].generation, 7u);
}

TEST_F(RenderPipelineAssetTest, NativeRegistryRoundTripPreservesEveryOwnedSetting)
{
    const auto source = Distinct();
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    const auto parsed = toml::parse(Text(File()));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.table()["schemaVersion"].value_or(0), 1);
    EXPECT_EQ(parsed.table()["render"]["mode"].value_or(std::string{}), "Hybrid");
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    ExpectOwned(pipeline->Settings(), source);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    const auto firstSave = Text(File());
    asset::DataAssetRegistry::ClearCache();
    pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    ExpectOwned(pipeline->Settings(), source);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(Text(File()), firstSave);
}

TEST_F(RenderPipelineAssetTest, CopyingEditorDiagnosticsPreservesRuntimeOwnedAndVolumeSettings)
{
    using Settings = renderer::RenderSettings;
    static constexpr bool Settings::* FLAGS[] = {
        &Settings::showColliders, &Settings::showTerrainCollision, &Settings::showDecalBounds,
        &Settings::showNavMesh, &Settings::showNavSensors, &Settings::showSkeleton,
        &Settings::skeletonSelectedOnly, &Settings::showGrid, &Settings::showLightRange,
        &Settings::showVFXGizmos, &Settings::showFlowFields, &Settings::showFlowSamples,
        &Settings::showPhysicsVolumes, &Settings::showWaterFlow, &Settings::showConstraints,
        &Settings::showRagdoll, &Settings::showScriptGizmos, &Settings::showRigidBodies,
        &Settings::showIK, &Settings::showSpringBones, &Settings::showAttachments,
        &Settings::showVFXPaths, &Settings::showTerrainBounds, &Settings::showLODBounds,
        &Settings::showUIRects, &Settings::showSelectionOutline, &Settings::passViewerEnabled,
        &Settings::particleOverdrawView, &Settings::particleOverdrawIncludeModels,
        &Settings::particleOverdrawReadback,
    };
    auto runtime = Distinct();
    runtime.userBrightness = 0.5f;
    runtime.renderScale = 0.75f;
    runtime.postProcess.exposure = 2;
    runtime.ssr.enabled = true;
    runtime.froxelFog.density = 0.125f;
    runtime.ibl.intensity = 9;
    runtime.objectMaskRequests = {{{5, 7}}};
    const auto before = runtime;
    Settings editor;
    for (auto flag : FLAGS) {
        runtime.*flag = false;
        editor.*flag = true;
    }
    editor.viewMode = renderer::ViewMode::RayInstanceId;
    editor.navMeshDrawMode = renderer::NavMeshDrawMode::Voxels;
    editor.navMeshDrawDistance = 69;
    editor.outlineWidth = 0.125f;
    for (int i = 0; i < 4; ++i) editor.outlineColor[i] = 0.125f * static_cast<float>(i + 1);
    editor.shadow.debugVisualizeCascades = true;
    editor.clustered.debugHeatmap = true;
    editor.clustered.forceAllLights = true;
    runtime.CopyDebugVisualizationFrom(editor);
    for (auto flag : FLAGS) EXPECT_TRUE(runtime.*flag);
    EXPECT_EQ(runtime.viewMode, renderer::ViewMode::RayInstanceId);
    EXPECT_EQ(runtime.navMeshDrawMode, renderer::NavMeshDrawMode::Voxels);
    EXPECT_FLOAT_EQ(runtime.navMeshDrawDistance, 69);
    EXPECT_FLOAT_EQ(runtime.outlineWidth, 0.125f);
    for (int i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(runtime.outlineColor[i], editor.outlineColor[i]);
    EXPECT_TRUE(runtime.shadow.debugVisualizeCascades);
    EXPECT_TRUE(runtime.clustered.debugHeatmap);
    EXPECT_TRUE(runtime.clustered.forceAllLights);
    ExpectOwned(runtime, before);
    EXPECT_FLOAT_EQ(runtime.userBrightness, 0.5f);
    EXPECT_FLOAT_EQ(runtime.renderScale, 0.75f);
    EXPECT_FLOAT_EQ(runtime.postProcess.exposure, 2);
    EXPECT_TRUE(runtime.ssr.enabled);
    EXPECT_FLOAT_EQ(runtime.froxelFog.density, 0.125f);
    EXPECT_FLOAT_EQ(runtime.ibl.intensity, 9);
    ASSERT_EQ(runtime.objectMaskRequests.size(), 1u);
    EXPECT_EQ(runtime.objectMaskRequests[0].id.generation, 7u);
    for (auto flag : FLAGS) editor.*flag = false;
    editor.viewMode = renderer::ViewMode::Lit;
    editor.shadow.debugVisualizeCascades = false;
    editor.clustered.debugHeatmap = false;
    editor.clustered.forceAllLights = false;
    runtime.CopyDebugVisualizationFrom(editor);
    for (auto flag : FLAGS) EXPECT_FALSE(runtime.*flag);
    EXPECT_EQ(runtime.viewMode, renderer::ViewMode::Lit);
    EXPECT_FALSE(runtime.shadow.debugVisualizeCascades);
    EXPECT_FALSE(runtime.clustered.debugHeatmap);
    EXPECT_FALSE(runtime.clustered.forceAllLights);
    ExpectOwned(runtime, before);
}

TEST_F(RenderPipelineAssetTest, EmptyOrMissingAssignmentReturnsTheUnchangedInlineFallback)
{
    auto fallback = Distinct();
    fallback.showColliders = true; fallback.userBrightness = 0.75f;
    renderer::RenderSettings resolved;
    EXPECT_FALSE(asset::ResolveRenderPipelineSettings(fallback, {}, resolved));
    ExpectOwned(resolved, fallback);
    EXPECT_TRUE(resolved.showColliders);
    EXPECT_FLOAT_EQ(resolved.userBrightness, 0.75f);
    EXPECT_FALSE(asset::ResolveRenderPipelineSettings(fallback, File("Missing.fzdata"), resolved));
    ExpectOwned(resolved, fallback);
}

TEST_F(RenderPipelineAssetTest, ResolvingThenDetachingDoesNotChangeTheInlineFallback)
{
    const auto source = Distinct();
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    renderer::RenderSettings fallback;
    fallback.userBloomScale = 0.5f;
    renderer::RenderSettings resolved;
    ASSERT_TRUE(asset::ResolveRenderPipelineSettings(fallback, File(), resolved));
    ExpectOwned(resolved, source);
    EXPECT_FLOAT_EQ(resolved.userBloomScale, 0.5f);
    EXPECT_EQ(fallback.pipeline, renderer::RenderingPipeline::Forward);
    EXPECT_TRUE(fallback.shadowEnabled);
    EXPECT_FALSE(asset::ResolveRenderPipelineSettings(fallback, {}, resolved));
    ExpectOwned(resolved, fallback);
}

TEST_F(RenderPipelineAssetTest, RemovingAGuidAssignedFileFallsBackUntilTheSameFileIsRestored)
{
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), Distinct()));
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
    const auto reference = asset::EncodeGuidRef(File());
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(reference));
    auto* pipeline = Resolve(reference);
    ASSERT_NE(pipeline, nullptr);
    const auto saved = Text(File());
    renderer::RenderSettings fallback;
    fallback.userBrightness = 0.75f;
    renderer::RenderSettings resolved;
    ASSERT_TRUE(asset::ResolveRenderPipelineSettings(fallback, reference, resolved));
    ExpectOwned(resolved, Distinct());
    std::error_code error;
    ASSERT_TRUE(std::filesystem::remove(File(), error));
    ASSERT_FALSE(error);
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 0);
    EXPECT_FALSE(asset::ResolveRenderPipelineSettings(fallback, reference, resolved));
    ExpectOwned(resolved, fallback);
    EXPECT_FLOAT_EQ(resolved.userBrightness, 0.75f);
    Write(File(), saved);
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    EXPECT_EQ(Resolve(reference), pipeline);
    ASSERT_TRUE(asset::ResolveRenderPipelineSettings(fallback, reference, resolved));
    ExpectOwned(resolved, Distinct());
}

TEST_F(RenderPipelineAssetTest, CreateNeverOverwritesExistingContentAndInvalidCaptureCreatesNothing)
{
    Write(File(), "existing bytes\n");
    EXPECT_FALSE(asset::CreateRenderPipelineAsset(File(), Distinct()));
    EXPECT_EQ(Text(File()), "existing bytes\n");
    auto invalid = Distinct();
    invalid.clustered.maxDistance = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(asset::CreateRenderPipelineAsset(File("Invalid.fzdata"), invalid));
    EXPECT_FALSE(std::filesystem::exists(File("Invalid.fzdata")));
    asset::RenderPipelineAsset asset;
    ASSERT_TRUE(asset.Capture(Distinct()));
    EXPECT_FALSE(asset.Capture(invalid));
    ExpectOwned(asset.Settings(), Distinct());
}

TEST_F(RenderPipelineAssetTest, WrongTypesUnknownSchemasAndInvalidNumericInputsFallBackWithoutWriting)
{
    const std::string prefix = "type='RenderPipelineAsset'\nschemaVersion=1\n";
    for (const std::string text : {std::string("type='RenderPipelineAsset'\nschemaVersion=2\n"),
             std::string("type='RenderPipelineAsset'\nschemaVersion='1'\n"),
             std::string("type='RenderPipelineAsset'\n"), prefix + "render=false\n",
             prefix + "[render]\nrayReflection=1\n", prefix + "[render]\nmode='Unknown'\n",
             prefix + "[shadow]\nmapResolution=-1\n", prefix + "[shadow]\nmapResolution=4294967296\n",
             prefix + "[shadow]\ncascadeCount=5\n", prefix + "[clustered]\nmaxDistance=nan\n",
             prefix + "[clustered]\nmaxDistance=1e300\n", prefix + "[froxel]\ngridZ=0\n",
             prefix + "[graph]\npassOverrides=[{name='A',extraReads=[1]}]\n"}) {
        SCOPED_TRACE(text);
        Write(File(), text);
        asset::DataAssetRegistry::ClearCache();
        renderer::RenderSettings out;
        EXPECT_FALSE(asset::ResolveRenderPipelineSettings(Distinct(), File(), out));
        ExpectOwned(out, Distinct());
        EXPECT_FALSE(asset::DataAssetRegistry::Save(File()));
        EXPECT_EQ(Text(File()), text);
    }
}

TEST_F(RenderPipelineAssetTest, HotReloadKeepsTheAddressAndLastGoodValuesOnTypedFailure)
{
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), Distinct()));
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=1\n[render]\npipeline='Deferred'\n");
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    EXPECT_EQ(Resolve(File()), pipeline);
    renderer::RenderSettings expected;
    expected.pipeline = renderer::RenderingPipeline::Deferred;
    ExpectOwned(pipeline->Settings(), expected);
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=1\n[render]\npipeline='Forward+'\n[shadow]\nmapResolution='bad'\n");
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 0);
    ExpectOwned(pipeline->Settings(), expected);
    renderer::RenderSettings resolved;
    ASSERT_TRUE(asset::ResolveRenderPipelineSettings(Distinct(), File(), resolved));
    ExpectOwned(resolved, expected);
    const auto malformed = Text(File());
    EXPECT_FALSE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(Text(File()), malformed);
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=2\nfuture='preserved'\n");
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 0);
    const auto future = Text(File());
    EXPECT_FALSE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(Text(File()), future);
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=1\n[render]\npipeline='Deferred'\n");
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    EXPECT_EQ(Resolve(File()), pipeline);
    EXPECT_TRUE(asset::DataAssetRegistry::Save(File()));
}

TEST_F(RenderPipelineAssetTest, SnapshotRestoreIsTypedAtomicAndPreservesTheSharedAddress)
{
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), Distinct()));
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    const auto snapshot = asset::DataAssetRegistry::Snapshot(File());
    ASSERT_FALSE(snapshot.empty());
    ASSERT_TRUE(pipeline->Capture(renderer::RenderSettings{}));
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(File(), snapshot));
    EXPECT_EQ(Resolve(File()), pipeline);
    ExpectOwned(pipeline->Settings(), Distinct());
    EXPECT_FALSE(asset::DataAssetRegistry::RestoreSnapshot(File(),
        "type='RenderPipelineAsset'\nschemaVersion=1\n[render]\npipeline='Forward'\n[shadow]\nmapResolution=false\n"));
    ExpectOwned(pipeline->Settings(), Distinct());
}

TEST_F(RenderPipelineAssetTest, UnknownSchemaOneFieldsSurviveKnownFieldEditsAndSave)
{
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=1\nfutureRoot='kept'\n[shadow]\nfutureTuning=7\n"
        "[graph]\npassOverrides=[{name='A',enabled=true,allowCulling=true,future=9}]\n");
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    auto edits = toml::parse("mapResolution=4096\npassOverrides=[{name='A',enabled=true,allowCulling=true}]\n");
    ASSERT_TRUE(edits);
    util::TomlReadReflector reflector(edits.table());
    pipeline->Reflect(reflector);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    const auto saved = toml::parse(Text(File()));
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved.table()["futureRoot"].value_or(std::string{}), "kept");
    EXPECT_EQ(saved.table()["shadow"]["futureTuning"].value_or(0), 7);
    EXPECT_EQ(saved.table()["shadow"]["mapResolution"].value_or(0), 4096);
    const auto* overrides = saved.table()["graph"]["passOverrides"].as_array();
    ASSERT_NE(overrides, nullptr);
    ASSERT_EQ(overrides->size(), 1u);
    const auto* pass = (*overrides)[0].as_table();
    ASSERT_NE(pass, nullptr);
    EXPECT_EQ((*pass)["future"].value_or(0), 9);
}

TEST_F(RenderPipelineAssetTest, GuidProjectReferenceRoundTripsWithoutLosingInlineSettings)
{
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), Distinct()));
    asset::AssetDatabase::Init(m_temp.File("Assets").generic_string());
    const auto reference = asset::EncodeGuidRef(File());
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(reference));
    ProjectSettings source;
    source.render.pipeline = renderer::RenderingPipeline::Deferred;
    source.render.modeRequest = {renderer::RenderMode::PATH_TRACING, renderer::PathTracingProfile::REFERENCE, false, true, false};
    source.render.shadow.mapResolution = 1024;
    source.render.particleBudget = 731;
    source.renderPipelineAssetPath = File();
    const auto project = m_temp.File("ProjectSettings.toml").generic_string();
    ASSERT_TRUE(source.Save(project));
    EXPECT_NE(Text(project).find("guid:"), std::string::npos);
    ProjectSettings restored;
    ASSERT_TRUE(restored.Load(project));
    EXPECT_EQ(restored.renderPipelineAssetPath, reference);
    EXPECT_EQ(restored.render.pipeline, source.render.pipeline);
    EXPECT_EQ(restored.render.modeRequest.mode, source.render.modeRequest.mode);
    EXPECT_EQ(restored.render.shadow.mapResolution, 1024u);
    EXPECT_EQ(restored.render.particleBudget, 731);
    renderer::RenderSettings resolved;
    ASSERT_TRUE(asset::ResolveRenderPipelineSettings(restored.render, restored.renderPipelineAssetPath, resolved));
    ExpectOwned(resolved, Distinct());
    restored.renderPipelineAssetPath.clear();
    ASSERT_TRUE(restored.Save(project));
    ASSERT_TRUE(restored.Load(project));
    EXPECT_TRUE(restored.renderPipelineAssetPath.empty());
    EXPECT_FALSE(asset::ResolveRenderPipelineSettings(restored.render, {}, resolved));
    EXPECT_EQ(resolved.pipeline, renderer::RenderingPipeline::Deferred);
    EXPECT_EQ(resolved.shadow.mapResolution, 1024u);
}

TEST_F(RenderPipelineAssetTest, ProjectTokenTypeFailurePreservesPreviousStateAndOldFilesClearTheAssignment)
{
    ProjectSettings settings;
    settings.render = Distinct();
    settings.renderPipelineAssetPath = "guid:unresolved|Assets/Missing.fzdata";
    const auto project = m_temp.File("ProjectSettings.toml").generic_string();
    Write(project, "[render]\npipelineAsset=3\npipeline='Forward'\n");
    EXPECT_FALSE(settings.Load(project));
    ExpectOwned(settings.render, Distinct());
    EXPECT_EQ(settings.renderPipelineAssetPath, "guid:unresolved|Assets/Missing.fzdata");
    Write(project, "[render]\npipeline='Deferred'\n");
    ASSERT_TRUE(settings.Load(project));
    EXPECT_TRUE(settings.renderPipelineAssetPath.empty());
    EXPECT_EQ(settings.render.pipeline, renderer::RenderingPipeline::Deferred);
}

TEST_F(RenderPipelineAssetTest, BuiltinRegistrationSurvivesScriptFactoryUnregistration)
{
    asset::DataAssetFactory::UnregisterScriptTypes();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(File(), asset::RenderPipelineAsset::TYPE_NAME));
    ASSERT_NE(Resolve(File()), nullptr);
}

TEST_F(RenderPipelineAssetTest, ReflectedListAdditionPersistsAndSnapshotUndoKeepsTheSharedAsset)
{
    const auto before = Distinct();
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), before));
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    const auto beforeSnapshot = asset::DataAssetRegistry::Snapshot(File());
    PassOverrideEditor editor;
    editor.additionalCount = 1;
    pipeline->Reflect(editor);
    auto after = before;
    after.passOverrides.push_back({"AddedPass", true, true, {}});
    ExpectOwned(pipeline->Settings(), after);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    const auto afterSnapshot = asset::DataAssetRegistry::Snapshot(File());
    ASSERT_FALSE(afterSnapshot.empty());
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(File(), beforeSnapshot));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(Resolve(File()), pipeline);
    ExpectOwned(pipeline->Settings(), before);
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(File(), afterSnapshot));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    EXPECT_EQ(Resolve(File()), pipeline);
    ExpectOwned(pipeline->Settings(), after);
}

TEST_F(RenderPipelineAssetTest, ReflectedListMovePreservesEveryFieldAndRejectsInvalidIndices)
{
    auto source = Distinct();
    source.passOverrides = {{"A", false, true, {"Depth"}}, {"B", true, false, {"Normal"}}, {"C", false, false, {"Color", "Depth"}}};
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    PassOverrideEditor editor;
    editor.requestMove = true;
    editor.moveFrom = 0;
    editor.moveTo = 2;
    pipeline->Reflect(editor);
    auto moved = source;
    moved.passOverrides = {source.passOverrides[1], source.passOverrides[2], source.passOverrides[0]};
    EXPECT_EQ(editor.moveQueries, 1u);
    ExpectOwned(pipeline->Settings(), moved);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    ExpectOwned(pipeline->Settings(), moved);
    editor.moveFrom = 2;
    editor.moveTo = 0;
    pipeline->Reflect(editor);
    ExpectOwned(pipeline->Settings(), source);
    editor.moveFrom = 3;
    editor.moveTo = 0;
    pipeline->Reflect(editor);
    ExpectOwned(pipeline->Settings(), source);
    editor.moveFrom = 0;
    editor.moveTo = 3;
    pipeline->Reflect(editor);
    ExpectOwned(pipeline->Settings(), source);
    editor.moveTo = 0;
    pipeline->Reflect(editor);
    ExpectOwned(pipeline->Settings(), source);
}

TEST_F(RenderPipelineAssetTest, ReflectedListDeletionPersistsAndSuppressesConcurrentMove)
{
    auto source = Distinct();
    source.passOverrides = {{"A", false, true, {"Depth"}}, {"B", true, false, {"Normal"}}, {"C", false, false, {"Color", "Depth"}}};
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    PassOverrideEditor editor;
    editor.removeIndex = 1;
    editor.requestMove = true;
    editor.moveFrom = 0;
    editor.moveTo = 2;
    pipeline->Reflect(editor);
    auto removed = source;
    removed.passOverrides = {source.passOverrides[0], source.passOverrides[2]};
    EXPECT_EQ(editor.moveQueries, 0u);
    ExpectOwned(pipeline->Settings(), removed);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    ExpectOwned(pipeline->Settings(), removed);
}

TEST_F(RenderPipelineAssetTest, DuplicateNameMovesAndDeletionsPreserveTheOriginalUnknownEntry)
{
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=1\n[graph]\npassOverrides=["
        "{name='Duplicate',enabled=false,future='first'},"
        "{name='Other',future='middle'},"
        "{name='Duplicate',allowCulling=false,future='second'}]\n");
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    const auto snapshot = asset::DataAssetRegistry::Snapshot(File());
    PassOverrideEditor move;
    move.requestMove = true;
    move.moveFrom = 0;
    move.moveTo = 2;
    pipeline->Reflect(move);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectPassUnknownFields({"middle", "second", "first"});
    PassOverrideEditor remove;
    remove.removeIndex = 1;
    pipeline->Reflect(remove);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectPassUnknownFields({"middle", "first"});
    EXPECT_FALSE(pipeline->Settings().passOverrides[1].enabled);
    PassOverrideEditor add;
    add.additionalCount = 1;
    add.renamedIndex = 2;
    add.renamedName = "Duplicate";
    pipeline->Reflect(add);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectPassUnknownFields({"middle", "first", nullptr});
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(File(), snapshot));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(Resolve(File()), pipeline);
    ExpectPassUnknownFields({"first", "middle", "second"});
    ASSERT_TRUE(pipeline->Capture(pipeline->Settings()));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectPassUnknownFields({nullptr, nullptr, nullptr});
}

TEST_F(RenderPipelineAssetTest, RenamingToAnExistingNameAndAddingAnOldNameNeverMixUnknownFields)
{
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=1\n[graph]\npassOverrides=["
        "{name='A',future='alpha'},{name='B',future='beta'}]\n");
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    PassOverrideEditor rename;
    rename.renamedIndex = 0;
    rename.renamedName = "B";
    pipeline->Reflect(rename);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectPassUnknownFields({nullptr, "beta"});
    PassOverrideEditor add;
    add.additionalCount = 1;
    add.renamedIndex = 2;
    add.renamedName = "A";
    pipeline->Reflect(add);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectPassUnknownFields({nullptr, "beta", nullptr});
    PassOverrideEditor remove;
    remove.removeIndex = 1;
    pipeline->Reflect(remove);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectPassUnknownFields({nullptr, nullptr});
}

TEST_F(RenderPipelineAssetTest, EmptyNameAdditionCanCommitOneRestorableSnapshotPairAfterValidInput)
{
    const auto source = Distinct();
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    const auto before = asset::DataAssetRegistry::Snapshot(File());
    ASSERT_FALSE(before.empty());
    const auto originalDisk = Text(File());
    PassOverrideEditor add;
    add.additionalCount = 1;
    add.newEntryName.clear();
    pipeline->Reflect(add);
    EXPECT_TRUE(asset::DataAssetRegistry::Snapshot(File()).empty());
    EXPECT_FALSE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(Text(File()), originalDisk);
    PassOverrideEditor name;
    name.renamedIndex = source.passOverrides.size();
    name.renamedName = "AddedPass";
    pipeline->Reflect(name);
    const auto after = asset::DataAssetRegistry::Snapshot(File());
    ASSERT_FALSE(after.empty());
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    auto expected = source;
    expected.passOverrides.push_back({"AddedPass", true, true, {}});
    ExpectOwned(pipeline->Settings(), expected);
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(File(), before));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ExpectOwned(pipeline->Settings(), source);
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(File(), after));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    EXPECT_EQ(Resolve(File()), pipeline);
    ExpectOwned(pipeline->Settings(), expected);
}

TEST_F(RenderPipelineAssetTest, HybridQualityUsesTheSameCodecForAssetAndInlineFallback)
{
    const auto source = Distinct();
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    const auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(pipeline->Settings().hybridQuality, source.hybridQuality);
    ProjectSettings settings;
    settings.render = source;
    const auto path = m_temp.File("ProjectSettings.toml").generic_string();
    ASSERT_TRUE(settings.Save(path));
    ProjectSettings loaded;
    ASSERT_TRUE(loaded.Load(path));
    EXPECT_EQ(loaded.render.hybridQuality, source.hybridQuality);
    auto parsed = toml::parse(Text(path));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.table()["render"]["hybrid"]["reflectionSamples"].value<int64_t>(), 2);
}

TEST_F(RenderPipelineAssetTest, InvalidHybridQualityRejectsWithoutChangingSettingsOrDisk)
{
    auto source = Distinct();
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    const auto disk = Text(File());
    source.hybridQuality.reflectionSamples = 0;
    EXPECT_FALSE(pipeline->Capture(source));
    EXPECT_EQ(Text(File()), disk);
    ExpectOwned(pipeline->Settings(), Distinct());
    const char* badValues[] = {"reflectionSamples=0", "reflectionSamples=65", "historyLimit=0",
        "spatialRadius=3", "glassBoundaryLimit=17", "maxHistoryMiB=0", "maxProbeCapturesPerFrame=17", "maxTraceDistance=-1",
        "frameBudgetMs=nan", "traceBudgetMs='3'", "probeUpdateBudgetMs=0", "reflectionResolutionDivisor=0",
        "reflectionResolutionDivisor=3", "glassStochastic=1", "glassStochastic='true'"};
    for (const char* bad : badValues) {
        const auto file = File("Bad.fzdata");
        Write(file, std::string("type='RenderPipelineAsset'\nschemaVersion=1\n[hybrid]\n") + bad + "\n");
        EXPECT_EQ(Resolve(file), nullptr) << bad;
        asset::DataAssetRegistry::ClearCache();
    }
    ProjectSettings existing;
    existing.game.project.name = "KeepThisProject";
    existing.render = Distinct();
    const auto projectFile = m_temp.File("BadSettings.toml").generic_string();
    Write(projectFile, "[project]\nname='MustNotApply'\n[render.hybrid]\nhistoryLimit=0\n");
    EXPECT_FALSE(existing.Load(projectFile));
    EXPECT_EQ(existing.game.project.name, "KeepThisProject");
    ExpectOwned(existing.render, Distinct());
    existing.render.hybridQuality.traceBudgetMs = -1;
    EXPECT_FALSE(existing.Save(projectFile));
    EXPECT_EQ(Text(projectFile), "[project]\nname='MustNotApply'\n[render.hybrid]\nhistoryLimit=0\n");
}

TEST_F(RenderPipelineAssetTest, LegacyHybridQualityDefaultsAndUnknownFieldsSurviveRoundTrip)
{
    Write(File(), "type='RenderPipelineAsset'\nschemaVersion=1\n[hybrid]\nfuturePolicy='keep'\n");
    const auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(pipeline->Settings().hybridQuality, renderer::HybridQualitySettings{});
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    auto parsed = toml::parse(Text(File()));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.table()["hybrid"]["futurePolicy"].value_or(std::string{}), "keep");
    ProjectSettings reused;
    reused.render.hybridQuality = Distinct().hybridQuality;
    const auto projectFile = m_temp.File("LegacySettings.toml").generic_string();
    Write(projectFile, "[render]\nmode='Hybrid'\nrayReflection=true\n");
    ASSERT_TRUE(reused.Load(projectFile));
    EXPECT_EQ(reused.render.hybridQuality, renderer::HybridQualitySettings{});
}

TEST_F(RenderPipelineAssetTest, PerformanceQualityRoundTripsInAssetAndProjectWithoutChangingSceneScale)
{
    auto source = Distinct();
    source.hybridQuality = renderer::MakeHybridQualityPreset(renderer::HybridQualityPreset::PERFORMANCE);
    ASSERT_TRUE(asset::CreateRenderPipelineAsset(File(), source));
    const auto* pipeline = Resolve(File());
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(pipeline->Settings().hybridQuality, source.hybridQuality);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    const auto assetText = toml::parse(Text(File()));
    ASSERT_TRUE(assetText);
    EXPECT_EQ(assetText.table()["hybrid"]["reflectionResolutionDivisor"].value<int64_t>(), 2);
    EXPECT_EQ(assetText.table()["hybrid"]["glassStochastic"].value<bool>(), true);
    ProjectSettings original;
    original.render = source;
    original.render.renderScale = 0.75f;
    const auto path = m_temp.File("PerformanceSettings.toml").generic_string();
    ASSERT_TRUE(original.Save(path));
    ProjectSettings loaded;
    loaded.render.renderScale = 0.75f;
    ASSERT_TRUE(loaded.Load(path));
    EXPECT_EQ(loaded.render.hybridQuality, source.hybridQuality);
    EXPECT_FLOAT_EQ(loaded.render.renderScale, 0.75f);
    const auto projectText = toml::parse(Text(path));
    ASSERT_TRUE(projectText);
    EXPECT_EQ(projectText.table()["render"]["hybrid"]["reflectionResolutionDivisor"].value<int64_t>(), 2);
    EXPECT_EQ(projectText.table()["render"]["hybrid"]["glassStochastic"].value<bool>(), true);
}

} /// @note namespace
} /// @note namespace fbzz::tests
