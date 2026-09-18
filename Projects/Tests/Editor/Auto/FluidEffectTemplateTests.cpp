/// @file    FluidEffectTemplateTests.cpp
/// @brief   複数素材の作成・時間差・素材参照と既存アセットの保護を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/FluidAssetWriters.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Fluid/FluidSolver.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fbzz::tests {
class FluidEffectTemplateTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
    }

    std::string Read(const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        std::ostringstream text;
        text << input.rdbuf();
        return text.str();
    }
};

TEST_F(FluidEffectTemplateTest, EveryTemplateCreatesIndependentRecipesAndReservesAllLayers)
{
    for (int i = 0; i < static_cast<int>(editor::FluidEffectTemplate::COUNT); ++i) {
        const auto preset = static_cast<editor::FluidEffectTemplate>(i);
        SCOPED_TRACE(editor::FluidEffectTemplateName(preset));
        const auto layers = editor::MakeFluidEffectLayers(preset);
        ASSERT_EQ(layers.size(), 3u);
        const std::string folder = "Assets/Effects/Pack" + std::to_string(i);
        editor::FluidBakeService service;
        editor::FluidJobError error;
        const auto id = service.EnqueueEffectTemplate(Context(), preset, folder, error);
        ASSERT_NE(id, 0u) << error.message;
        ASSERT_NE(service.Find(id), nullptr);
        EXPECT_EQ(service.Find(id)->state, editor::FluidJobState::Queued);
        for (std::size_t j = 0; j < layers.size(); ++j) {
            const auto path = ProjectRoot() / folder / (layers[j].name + ".fluid");
            fluid::FluidRecipe recipe;
            ASSERT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(path), recipe));
            EXPECT_FALSE(recipe.output.loop);
            EXPECT_EQ(recipe.bake.mode, fluid::FluidBakeMode::Flat2D);
            EXPECT_GT(recipe.output.duration, 0.0f);
            EXPECT_FALSE(recipe.sources.empty());
            EXPECT_TRUE(service.IsBusy(util::FileSystem::PathToUtf8(path)));
            if (j > 0) EXPECT_GT(layers[j].startDelay, layers[j - 1].startDelay);
        }
        EXPECT_TRUE(service.Cancel(id));
        EXPECT_EQ(service.Find(id)->state, editor::FluidJobState::Cancelled);
        EXPECT_FALSE(service.IsAnyBusy());
        EXPECT_EQ(service.EnqueueEffectTemplate(Context(), preset, folder, error), 0u);
        EXPECT_EQ(error.code, "ALREADY_EXISTS");
        EXPECT_FALSE(std::filesystem::exists(ProjectRoot() / folder / ("Pack" + std::to_string(i) + ".vfx")));
    }
}

TEST_F(FluidEffectTemplateTest, RejectsTraversalAndUnknownTemplateBeforeWriting)
{
    editor::FluidBakeService service;
    editor::FluidJobError error;
    EXPECT_EQ(service.EnqueueEffectTemplate(Context(), editor::FluidEffectTemplate::LANDING,
                                           "Assets/../OutsideAssets", error), 0u);
    EXPECT_EQ(error.code, "BAD_PATH");
    EXPECT_FALSE(std::filesystem::exists(ProjectRoot() / "OutsideAssets"));
    EXPECT_EQ(service.EnqueueEffectTemplate(Context(), editor::FluidEffectTemplate::COUNT,
                                           "Assets/Invalid", error), 0u);
    EXPECT_FALSE(std::filesystem::exists(ProjectRoot() / "Assets/Invalid"));
}

TEST_F(FluidEffectTemplateTest, VfxPreservesLayerTimingSingleBurstsAndDistinctMaterials)
{
    const auto layers = editor::MakeFluidEffectLayers(editor::FluidEffectTemplate::CHARGE_RELEASE);
    std::vector<std::string> materials;
    for (const auto& layer : layers) {
        const auto path = File(layer.name + ".mat");
        const std::string name = util::FileSystem::PathToUtf8(path);
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(name, editor::NewFluidParticleMaterial()));
        materials.push_back(name);
    }
    const auto file = File("Effect.vfx");
    std::string error;
    ASSERT_TRUE(editor::WriteLayeredFluidVfx(file, "Charge \"A\"", layers, materials, error)) << error;
    const std::string original = Read(file);
    const auto parsed = toml::parse(original);
    ASSERT_TRUE(parsed);
    const auto* objects = parsed.table()["gameobjects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_EQ(objects->size(), layers.size() + 1);
    const auto& root = *(*objects)[0].as_table();
    EXPECT_FALSE(root["VFXComponent"]["loop"].value_or(true));
    std::vector<std::string> references;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const auto* object = (*objects)[i + 1].as_table();
        ASSERT_NE(object, nullptr);
        EXPECT_EQ((*object)["parentInstanceId"].value_or(std::string{}), root["instanceId"].value_or(std::string{}));
        const auto emitter = (*object)["ParticleEmitter"];
        EXPECT_NEAR(emitter["startDelay"].value_or(-1.0), layers[i].startDelay, 1.0e-5);
        EXPECT_NEAR(emitter["lifetime"].value_or(-1.0), layers[i].recipe.output.duration, 1.0e-5);
        EXPECT_FALSE(emitter["loop"].value_or(true));
        EXPECT_DOUBLE_EQ(emitter["emitRate"].value_or(-1.0), 0.0);
        const auto* bursts = emitter["bursts"].as_array();
        ASSERT_NE(bursts, nullptr);
        ASSERT_EQ(bursts->size(), 1u);
        ASSERT_NE((*bursts)[0].as_table(), nullptr);
        EXPECT_EQ((*(*bursts)[0].as_table())["count"].value_or(0), 1);
        references.push_back(emitter["materialPath"].value_or(std::string{}));
        EXPECT_FALSE(references.back().empty());
    }
    EXPECT_NE(references[0], references[1]);
    EXPECT_NE(references[1], references[2]);
    EXPECT_FALSE(editor::WriteLayeredFluidVfx(file, "Replacement", layers, materials, error));
    EXPECT_EQ(Read(file), original);
    materials.pop_back();
    EXPECT_FALSE(editor::WriteLayeredFluidVfx(File("Incomplete.vfx"), "Incomplete", layers, materials, error));
    EXPECT_FALSE(std::filesystem::exists(File("Incomplete.vfx")));
}
TEST_F(FluidEffectTemplateTest, NewMaterialPresetsRoundTripAndInjectFiniteDensity)
{
    for (auto preset : { asset::FluidPreset::GroundRing, asset::FluidPreset::ColdMist,
                         asset::FluidPreset::ChargeVortex, asset::FluidPreset::EmberBurst,
                         asset::FluidPreset::SigilFlare }) {
        SCOPED_TRACE(asset::FluidPresetName(preset));
        const auto recipe = asset::MakeFluidPreset(preset);
        const auto file = File("Material.fluid");
        ASSERT_TRUE(asset::SaveFluidRecipe(util::FileSystem::PathToUtf8(file), recipe));
        fluid::FluidRecipe loaded;
        ASSERT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(file), loaded));
        EXPECT_EQ(loaded.sources.size(), recipe.sources.size());
        EXPECT_EQ(loaded.forces.size(), recipe.forces.size());
        EXPECT_EQ(loaded.bake.mode, fluid::FluidBakeMode::Volume3D);
        EXPECT_FALSE(loaded.output.loop);
        fluid::FluidGasSolver solver;
        solver.Reset(loaded, 32, 32, 1);
        for (int i = 0; i < 12; ++i) solver.Advance(1.0f / 120.0f);
        EXPECT_GT(solver.TotalDensity(), 0.0f);
        for (float density : solver.Density()) EXPECT_TRUE(std::isfinite(density));
    }
}

} // namespace fbzz::tests
