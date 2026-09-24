/// @file    FluidRecipeBakeTests.cpp
/// @brief   .fluid の [bake]・部品 ([[source]] / [[force]] / [[collider]])・ReflectFluidRecipe の項目名・3D 焼き設定への写し・テクスチャ発生源のマスクを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note AI は ReflectFluidRecipe のフィールド名で .fluid を書き換え、焼くときは TOML を読み直す。
/// @note 名前が 1 つずれるだけで «書いたのに効かない» になり、エラーはどこにも出ない。

#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/FluidFireRendering.hpp>
#include <Engine/Asset/FluidSourceMaskLoader.hpp>
#include <Engine/Asset/FluidVolumeBake.hpp>
#include <Engine/Asset/VolumeFlipbookFluid.hpp>
#include <Engine/Asset/VolumeFlipbookBaker.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Fluid/FluidStepping.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <toml++/toml.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

using KeyValue = std::pair<std::string, std::string>;

/// @brief 訪れたフィールドをパス表記で集め、Enum は選ばれている名前も集める。
class KeyRecorder final : public scene::IReflector {
public:
    std::set<std::string> keys;
    std::set<KeyValue> enumValues;

    void Field(const char* name, float&) override { Add(name); }
    void Field(const char* name, int&) override { Add(name); }
    void Field(const char* name, bool&) override { Add(name); }
    void Field(const char* name, math::Vector2&) override { Add(name); }
    void Field(const char* name, math::Vector3&) override { Add(name); }
    void Field(const char* name, math::Vector4&) override { Add(name); }
    void Field(const char* name, std::string&) override { Add(name); }
    void Field(const char* name, math::Quaternion&) override { Add(name); }
    void Enum(const char* name, int& v, std::span<const char* const> labels) override
    {
        Add(name);
        if (v >= 0 && static_cast<std::size_t>(v) < labels.size())
            enumValues.emplace(Path(name), labels[static_cast<std::size_t>(v)]);
    }
    void BeginObject(const char* name) override { m_prefix.push_back(Path(name) + "."); }
    void EndObject() override { m_prefix.pop_back(); }
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        m_prefix.push_back(Path(name) + "[].");
        return count;
    }
    std::size_t EndObjectList() override
    {
        m_prefix.pop_back();
        return NO_REMOVE;
    }

private:
    std::string Path(const char* name) const
    {
        return (m_prefix.empty() ? std::string{} : m_prefix.back()) + PersistentKey(name);
    }
    void Add(const char* name) { keys.insert(Path(name)); }

    std::vector<std::string> m_prefix;
};

void CollectTomlKeys(const toml::table& table, const std::string& prefix, std::set<std::string>& keys,
                     std::set<KeyValue>& strings)
{
    for (const auto& [key, node] : table) {
        const std::string path = prefix + std::string(key);
        if (const toml::table* child = node.as_table()) {
            CollectTomlKeys(*child, path + ".", keys, strings);
            continue;
        }
        const toml::array* array = node.as_array();
        if (array != nullptr && !array->empty() && (*array)[0].is_table()) {
            for (const toml::node& element : *array)
                if (const toml::table* elementTable = element.as_table())
                    CollectTomlKeys(*elementTable, path + "[].", keys, strings);
            continue;
        }
        keys.insert(path);
        if (const auto text = node.value<std::string>()) strings.emplace(path, *text);
    }
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

std::set<std::string> Difference(const std::set<std::string>& a, const std::set<std::string>& b)
{
    std::set<std::string> result;
    for (const std::string& key : a)
        if (b.count(key) == 0) result.insert(key);
    return result;
}

fluid::FluidBakeSettings NonDefaultBake()
{
    fluid::FluidBakeSettings bake;
    bake.mode = fluid::FluidBakeMode::Volume3D;
    bake.volumeResolution = 96;
    bake.raySteps = 256;
    bake.shadowSteps = 24;
    bake.solver = fluid::FluidBakeSolver::Cpu;
    bake.densityScale = 2.5f;
    bake.scatteringOctaves = 5;
    bake.skyOcclusion = 0.25f;
    bake.blackbodyEmission = true;
    bake.blackbodyMinKelvin = 1200.0f;
    bake.blackbodyMaxKelvin = 3100.0f;
    bake.sixWayLightmaps = true;
    bake.lightYawDegrees = -20.0f;
    bake.lightPitchDegrees = 70.0f;
    bake.lightColor = { 1.5f, 1.25f, 1.0f };
    bake.ambient = { 0.1f, 0.2f, 0.3f };
    bake.extinction = 14.0f;
    bake.anisotropy = -0.2f;
    bake.emissionIntensity = 9.0f;
    bake.exposure = 1.3f;
    bake.cameraYawDegrees = 45.0f;
    bake.halfExtent = 1.4f;
    return bake;
}

void ExpectVector3Eq(const math::Vector3& actual, const math::Vector3& expected)
{
    EXPECT_FLOAT_EQ(actual.x, expected.x);
    EXPECT_FLOAT_EQ(actual.y, expected.y);
    EXPECT_FLOAT_EQ(actual.z, expected.z);
}

void ExpectBakeEq(const fluid::FluidBakeSettings& actual, const fluid::FluidBakeSettings& expected)
{
    EXPECT_EQ(actual.mode, expected.mode);
    EXPECT_EQ(actual.volumeResolution, expected.volumeResolution);
    EXPECT_EQ(actual.raySteps, expected.raySteps);
    EXPECT_EQ(actual.shadowSteps, expected.shadowSteps);
    EXPECT_EQ(actual.solver, expected.solver);
    EXPECT_FLOAT_EQ(actual.densityScale, expected.densityScale);
    EXPECT_EQ(actual.scatteringOctaves, expected.scatteringOctaves);
    EXPECT_FLOAT_EQ(actual.skyOcclusion, expected.skyOcclusion);
    EXPECT_EQ(actual.blackbodyEmission, expected.blackbodyEmission);
    EXPECT_FLOAT_EQ(actual.blackbodyMinKelvin, expected.blackbodyMinKelvin);
    EXPECT_FLOAT_EQ(actual.blackbodyMaxKelvin, expected.blackbodyMaxKelvin);
    EXPECT_EQ(actual.sixWayLightmaps, expected.sixWayLightmaps);
    EXPECT_FLOAT_EQ(actual.lightYawDegrees, expected.lightYawDegrees);
    EXPECT_FLOAT_EQ(actual.lightPitchDegrees, expected.lightPitchDegrees);
    ExpectVector3Eq(actual.lightColor, expected.lightColor);
    ExpectVector3Eq(actual.ambient, expected.ambient);
    EXPECT_FLOAT_EQ(actual.extinction, expected.extinction);
    EXPECT_FLOAT_EQ(actual.anisotropy, expected.anisotropy);
    EXPECT_FLOAT_EQ(actual.emissionIntensity, expected.emissionIntensity);
    EXPECT_FLOAT_EQ(actual.exposure, expected.exposure);
    EXPECT_FLOAT_EQ(actual.cameraYawDegrees, expected.cameraYawDegrees);
    EXPECT_FLOAT_EQ(actual.halfExtent, expected.halfExtent);
}

void ExpectMotionEq(const fluid::FluidMotion& actual, const fluid::FluidMotion& expected)
{
    EXPECT_EQ(actual.inheritVelocity, expected.inheritVelocity);
    ASSERT_EQ(actual.keys.size(), expected.keys.size());
    for (std::size_t i = 0; i < actual.keys.size(); ++i) {
        EXPECT_FLOAT_EQ(actual.keys[i].time, expected.keys[i].time);
        ExpectVector3Eq(actual.keys[i].offset, expected.keys[i].offset);
    }
}

void ExpectAmountEq(const fluid::FluidAmount& actual, const fluid::FluidAmount& expected)
{
    ASSERT_EQ(actual.keys.size(), expected.keys.size());
    for (std::size_t i = 0; i < actual.keys.size(); ++i) {
        EXPECT_FLOAT_EQ(actual.keys[i].time, expected.keys[i].time);
        EXPECT_FLOAT_EQ(actual.keys[i].scale, expected.keys[i].scale);
    }
}

void ExpectSourceEq(const fluid::FluidSource& actual, const fluid::FluidSource& expected)
{
    EXPECT_EQ(actual.enabled, expected.enabled);
    EXPECT_EQ(actual.name, expected.name);
    EXPECT_EQ(actual.shape, expected.shape);
    ExpectVector3Eq(actual.center, expected.center);
    ExpectVector3Eq(actual.size, expected.size);
    ExpectVector3Eq(actual.direction, expected.direction);
    EXPECT_EQ(actual.texture, expected.texture);
    EXPECT_FLOAT_EQ(actual.density, expected.density);
    EXPECT_FLOAT_EQ(actual.temperature, expected.temperature);
    EXPECT_FLOAT_EQ(actual.fuel, expected.fuel);
    EXPECT_FLOAT_EQ(actual.noise, expected.noise);
    ExpectVector3Eq(actual.velocity, expected.velocity);
    EXPECT_FLOAT_EQ(actual.startTime, expected.startTime);
    EXPECT_FLOAT_EQ(actual.duration, expected.duration);
    EXPECT_FLOAT_EQ(actual.colorKey, expected.colorKey);
    EXPECT_FLOAT_EQ(actual.spread, expected.spread);
    EXPECT_EQ(actual.count, expected.count);
    ExpectMotionEq(actual.motion, expected.motion);
    ExpectAmountEq(actual.amount, expected.amount);
}

void ExpectForceEq(const fluid::FluidForce& actual, const fluid::FluidForce& expected)
{
    EXPECT_EQ(actual.enabled, expected.enabled);
    EXPECT_EQ(actual.name, expected.name);
    EXPECT_EQ(actual.type, expected.type);
    ExpectVector3Eq(actual.center, expected.center);
    ExpectVector3Eq(actual.direction, expected.direction);
    EXPECT_FLOAT_EQ(actual.strength, expected.strength);
    EXPECT_FLOAT_EQ(actual.radius, expected.radius);
    EXPECT_FLOAT_EQ(actual.falloffPower, expected.falloffPower);
    EXPECT_FLOAT_EQ(actual.noiseFrequency, expected.noiseFrequency);
    EXPECT_FLOAT_EQ(actual.noiseSpeed, expected.noiseSpeed);
    EXPECT_FLOAT_EQ(actual.startTime, expected.startTime);
    EXPECT_FLOAT_EQ(actual.duration, expected.duration);
    ExpectMotionEq(actual.motion, expected.motion);
    ExpectAmountEq(actual.amount, expected.amount);
}

void ExpectColliderEq(const fluid::FluidCollider& actual, const fluid::FluidCollider& expected)
{
    EXPECT_EQ(actual.enabled, expected.enabled);
    EXPECT_EQ(actual.name, expected.name);
    EXPECT_EQ(actual.shape, expected.shape);
    ExpectVector3Eq(actual.center, expected.center);
    ExpectVector3Eq(actual.size, expected.size);
    ExpectVector3Eq(actual.direction, expected.direction);
    EXPECT_FLOAT_EQ(actual.friction, expected.friction);
    EXPECT_FLOAT_EQ(actual.startTime, expected.startTime);
    EXPECT_FLOAT_EQ(actual.duration, expected.duration);
    ExpectMotionEq(actual.motion, expected.motion);
}

void ExpectRampEq(const fluid::FluidColorRamp& actual, const fluid::FluidColorRamp& expected)
{
    for (std::size_t i = 0; i < actual.stops.size(); ++i) {
        ExpectVector3Eq(actual.stops[i].color, expected.stops[i].color);
        EXPECT_FLOAT_EQ(actual.stops[i].position, expected.stops[i].position);
    }
}

/// @brief 保存して読み直す。
fluid::FluidRecipe RoundTrip(const testkit::TempDir& temp, const char* fileName, const fluid::FluidRecipe& recipe)
{
    const std::string path = util::FileSystem::PathToUtf8(temp.File(fileName));
    EXPECT_TRUE(asset::SaveFluidRecipe(path, recipe));
    fluid::FluidRecipe loaded;
    EXPECT_TRUE(asset::LoadFluidRecipe(path, loaded));
    return loaded;
}

/// @brief TOML の文字列をファイルに書いて読む。
fluid::FluidRecipe LoadText(const testkit::TempDir& temp, const char* fileName, const std::string& text)
{
    const std::filesystem::path file = temp.File(fileName);
    EXPECT_TRUE(util::FileSystem::WriteText(file, text));
    fluid::FluidRecipe loaded;
    EXPECT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(file), loaded));
    return loaded;
}

float ToLinear(float c) { return std::pow(c, 2.2f); }

/// @note 無圧縮 32bit の TGA を書く (rgba は上の行から)。stb_image が読める一番単純な形で、PNG の符号化器を要らなくする。
bool WriteTgaRgba8(const std::filesystem::path& file, int width, int height, const std::vector<std::uint8_t>& rgba)
{
    std::vector<std::uint8_t> bytes(18, 0);
    /// @note 無圧縮 true color
    bytes[2] = 2;
    bytes[12] = static_cast<std::uint8_t>(width & 0xFF);
    bytes[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);
    bytes[14] = static_cast<std::uint8_t>(height & 0xFF);
    bytes[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    bytes[16] = 32;
    /// @note α 8bit + 原点は左上
    bytes[17] = 0x28;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        /// @note TGA は BGRA
        bytes.push_back(rgba[i + 2]);
        bytes.push_back(rgba[i + 1]);
        bytes.push_back(rgba[i + 0]);
        bytes.push_back(rgba[i + 3]);
    }
    return util::FileSystem::WriteBinary(file, bytes.data(), bytes.size());
}

}

TEST(FluidRecipeBakeTest, BakeSectionRoundTripsThroughToml)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.bake = NonDefaultBake();
    const std::string path = util::FileSystem::PathToUtf8(temp.File("smoke.fluid"));
    ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
    fluid::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(path, loaded));
    ExpectBakeEq(loaded.bake, recipe.bake);
}

TEST(FluidRecipeBakeTest, FileWithoutBakeSectionLoadsAs2D)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    const std::filesystem::path file = temp.File("old.fluid");
    ASSERT_TRUE(util::FileSystem::WriteText(file, "version = 1\nkind = \"gas\"\n\n[output]\ncolumns = 4\n"));
    fluid::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(file), loaded));
    EXPECT_EQ(loaded.output.columns, 4);
    EXPECT_EQ(loaded.bake.mode, fluid::FluidBakeMode::Flat2D);
    ExpectBakeEq(loaded.bake, fluid::FluidBakeSettings{});
}

TEST(FluidRecipeBakeTest, AllPresetsBakeIn3DAndLiquidsStayOnCpu)
{
    /// @note 3D の焼きがループと歪みマップを持ったので、ループものも陽炎も 3D。
    /// @note 液体は GPU の粒子ソルバーが未検証のうちは CPU に置く。
    for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i) {
        const auto preset = static_cast<asset::FluidPreset>(i);
        SCOPED_TRACE(asset::FluidPresetName(preset));
        const fluid::FluidRecipe recipe = asset::MakeFluidPreset(preset);
        EXPECT_EQ(recipe.bake.mode, fluid::FluidBakeMode::Volume3D);
        if (recipe.kind == fluid::FluidKind::Liquid) {
            EXPECT_EQ(recipe.bake.solver, fluid::FluidBakeSolver::Cpu);
            EXPECT_EQ(recipe.bake.volumeResolution, 64);
            /// @note CPU の上限
            EXPECT_LE(recipe.bake.volumeResolution, 96);
            EXPECT_EQ(recipe.output.supersampling, 2);
        } else {
            EXPECT_EQ(recipe.bake.volumeResolution, 128);
            EXPECT_EQ(recipe.output.supersampling, 2);
            const bool lit = recipe.render.shading == fluid::FluidShading::Smoke
                          || recipe.render.shading == fluid::FluidShading::Fire;
            EXPECT_EQ(recipe.bake.sixWayLightmaps, lit);
        }
    }
    EXPECT_TRUE(asset::MakeFluidPreset(asset::FluidPreset::Fire).bake.blackbodyEmission);
    EXPECT_TRUE(asset::MakeFluidPreset(asset::FluidPreset::Smoke).bake.sixWayLightmaps);
    EXPECT_FALSE(asset::MakeFluidPreset(asset::FluidPreset::HeatHaze).bake.sixWayLightmaps);
}

TEST(FluidRecipeBakeTest, ReflectNamesAreTheTomlKeys)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    /// @note 配列は要素が無いとキーが出ないので、発生源・力・動き・量のキーすべてに 1 つ以上入れる。
    /// @note Enum は既定以外の値にする。
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    fluid::FluidSource& moving = recipe.sources.emplace_back();
    moving.name = "Moving";
    moving.shape = fluid::FluidSourceShape::Box;
    moving.motion.keys = { fluid::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                           fluid::FluidMotionKey{ 1.0f, { 0.2f, 0.0f, 0.0f } } };
    moving.amount.keys = { fluid::FluidAmountKey{ 0.0f, 1.0f }, fluid::FluidAmountKey{ 0.4f, 0.25f } };
    fluid::FluidForce& gust = recipe.forces.emplace_back();
    gust.name = "Gust";
    gust.type = fluid::FluidForceType::Noise;
    gust.motion.keys = { fluid::FluidMotionKey{ 0.5f, { 0.0f, 0.1f, 0.0f } } };
    gust.amount.keys = { fluid::FluidAmountKey{ 0.2f, 0.5f } };
    fluid::FluidSource& stamp = recipe.sources.emplace_back();
    stamp.name = "Stamp";
    stamp.shape = fluid::FluidSourceShape::Texture;
    stamp.texture = "Textures/FX/Logo.png";
    fluid::FluidCollider& wall = recipe.colliders.emplace_back();
    wall.name = "Wall";
    wall.shape = fluid::FluidColliderShape::Plane;
    wall.motion.keys = { fluid::FluidMotionKey{ 0.25f, { 0.1f, 0.0f, 0.0f } } };
    recipe.kind = fluid::FluidKind::Liquid;
    recipe.render.shading = fluid::FluidShading::Glow;
    recipe.render.useEmissionRamp = true;
    recipe.render.useAlbedoRamp = true;
    recipe.bake = NonDefaultBake();

    const std::filesystem::path file = temp.File("keys.fluid");
    ASSERT_TRUE(asset::SaveFluidRecipe(util::FileSystem::PathToUtf8(file), recipe));
    std::ifstream stream(file, std::ios::binary);
    ASSERT_TRUE(stream.good());
    std::stringstream text;
    text << stream.rdbuf();
    toml::parse_result parsed = toml::parse(text.str());
    ASSERT_TRUE(parsed) << ".fluid の TOML が壊れています";

    std::set<std::string> tomlKeys;
    std::set<KeyValue> tomlStrings;
    CollectTomlKeys(parsed.table(), {}, tomlKeys, tomlStrings);
    tomlKeys.erase("version");
    /// @note 部品の name / texture は自由な文字列で Enum ではない。選択肢の比較からは外す。
    std::erase_if(tomlStrings, [](const KeyValue& entry) {
        return entry.first.ends_with("[].name") || entry.first.ends_with("[].texture");
    });

    KeyRecorder recorder;
    asset::ReflectFluidRecipe(recipe, recorder);

    EXPECT_TRUE(Difference(recorder.keys, tomlKeys).empty())
        << "TOML に無い Reflect 名: " << Join(Difference(recorder.keys, tomlKeys));
    EXPECT_TRUE(Difference(tomlKeys, recorder.keys).empty())
        << "Reflect が触らない TOML キー: " << Join(Difference(tomlKeys, recorder.keys));
    /// @note Enum の選択肢の文字列 = TOML に書く名前 (JSON は添字、TOML は名前で同じ値を指す)。
    EXPECT_EQ(recorder.enumValues, tomlStrings);
    /// @note 部品の配列は期待した名前で出ている (上の一致だけだと、両方が同じ名前で間違っていても通る)。
    EXPECT_EQ(tomlKeys.count("source[].motion.key[].offset"), 1u);
    EXPECT_EQ(tomlKeys.count("force[].motion.key[].time"), 1u);
    EXPECT_EQ(tomlKeys.count("render.emission_ramp[].color"), 1u);
    EXPECT_EQ(tomlKeys.count("render.use_albedo_ramp"), 1u);
    EXPECT_EQ(tomlKeys.count("render.albedo_ramp[].color"), 1u);
    EXPECT_EQ(tomlKeys.count("render.albedo_ramp[].position"), 1u);
    EXPECT_EQ(tomlKeys.count("source[].color_key"), 1u);
    EXPECT_EQ(tomlKeys.count("source[].texture"), 1u);
    EXPECT_EQ(tomlKeys.count("collider[].friction"), 1u);
    EXPECT_EQ(tomlKeys.count("collider[].motion.key[].offset"), 1u);
    EXPECT_EQ(tomlKeys.count("source[].amount.key[].time"), 1u);
    EXPECT_EQ(tomlKeys.count("source[].amount.key[].scale"), 1u);
    EXPECT_EQ(tomlKeys.count("force[].amount.key[].scale"), 1u);
}

TEST(FluidRecipeBakeTest, SourcesForcesMotionAndRampRoundTrip)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    fluid::FluidRecipe recipe;
    fluid::FluidSource ring;
    ring.enabled = false;
    ring.name = "Shock";
    ring.shape = fluid::FluidSourceShape::Ring;
    ring.center = { 0.1f, -0.2f, 0.3f };
    ring.size = { 0.4f, 0.05f, 0.0f };
    ring.direction = { 0.0f, 0.0f, 1.0f };
    ring.density = 7.5f;
    ring.temperature = 0.25f;
    ring.fuel = 1.5f;
    ring.noise = 0.1f;
    ring.velocity = { 0.5f, 0.25f, -0.5f };
    ring.startTime = 0.2f;
    ring.duration = 0.8f;
    ring.spread = 0.9f;
    ring.count = 1234;
    ring.colorKey = 0.75f;
    ring.motion.inheritVelocity = false;
    ring.motion.keys = { fluid::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                         fluid::FluidMotionKey{ 0.5f, { 0.3f, 0.1f, 0.0f } },
                         fluid::FluidMotionKey{ 1.5f, { -0.2f, 0.4f, 0.1f } } };
    recipe.sources.push_back(ring);
    fluid::FluidSource cone;
    cone.shape = fluid::FluidSourceShape::Cone;
    recipe.sources.push_back(cone);

    fluid::FluidForce vortex;
    vortex.name = "Spin";
    vortex.type = fluid::FluidForceType::Vortex;
    vortex.center = { -0.3f, 0.2f, 0.0f };
    vortex.direction = { 0.0f, 1.0f, 0.0f };
    vortex.strength = -3.5f;
    vortex.radius = 0.6f;
    vortex.falloffPower = 1.5f;
    vortex.noiseFrequency = 5.0f;
    vortex.noiseSpeed = 0.25f;
    vortex.startTime = 0.1f;
    vortex.duration = 2.0f;
    vortex.motion.keys = { fluid::FluidMotionKey{ 0.25f, { 0.0f, 0.5f, 0.0f } } };
    recipe.forces.push_back(vortex);
    fluid::FluidForce drag;
    drag.type = fluid::FluidForceType::Drag;
    recipe.forces.push_back(drag);

    recipe.render.useEmissionRamp = true;
    recipe.render.emissionRamp.stops[1] = { { 0.2f, 0.4f, 1.0f }, 0.25f };
    recipe.render.emissionRamp.stops[3] = { { 8.0f, 6.0f, 2.0f }, 0.9f };
    recipe.render.useAlbedoRamp = true;
    recipe.render.albedoRamp.stops[0] = { { 0.9f, 0.1f, 0.05f }, 0.0f };
    recipe.render.albedoRamp.stops[2] = { { 0.1f, 0.3f, 0.8f }, 0.6f };
    recipe.render.liquidSoftness = 0.15f;
    recipe.render.liquidExtinction = 25.0f;
    recipe.render.liquidGloss = 200.0f;
    recipe.render.liquidFresnel = 0.05f;

    const fluid::FluidRecipe loaded = RoundTrip(temp, "parts.fluid", recipe);
    ASSERT_EQ(loaded.sources.size(), recipe.sources.size());
    for (std::size_t i = 0; i < loaded.sources.size(); ++i) ExpectSourceEq(loaded.sources[i], recipe.sources[i]);
    ASSERT_EQ(loaded.forces.size(), recipe.forces.size());
    for (std::size_t i = 0; i < loaded.forces.size(); ++i) ExpectForceEq(loaded.forces[i], recipe.forces[i]);
    EXPECT_TRUE(loaded.render.useEmissionRamp);
    ExpectRampEq(loaded.render.emissionRamp, recipe.render.emissionRamp);
    EXPECT_TRUE(loaded.render.useAlbedoRamp);
    ExpectRampEq(loaded.render.albedoRamp, recipe.render.albedoRamp);
    EXPECT_FLOAT_EQ(loaded.render.liquidSoftness, 0.15f);
    EXPECT_FLOAT_EQ(loaded.render.liquidExtinction, 25.0f);
    EXPECT_FLOAT_EQ(loaded.render.liquidGloss, 200.0f);
    EXPECT_FLOAT_EQ(loaded.render.liquidFresnel, 0.05f);
}

TEST(FluidRecipeBakeTest, CollidersAndTextureSourcesRoundTrip)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    fluid::FluidRecipe recipe;
    fluid::FluidSource stamp;
    stamp.name = "Sigil";
    stamp.shape = fluid::FluidSourceShape::Texture;
    stamp.texture = "guid:0123456789abcdef0123456789abcdef";
    stamp.size = { 0.5f, 0.25f, 0.04f };
    stamp.direction = { 0.0f, 0.0f, 1.0f };
    recipe.sources.push_back(stamp);

    fluid::FluidCollider ball;
    ball.enabled = false;
    ball.name = "Ball";
    ball.center = { 0.1f, 0.2f, -0.3f };
    ball.size = { 0.3f, 0.0f, 0.0f };
    ball.friction = 0.9f;
    ball.startTime = 0.25f;
    ball.duration = 1.5f;
    ball.motion.inheritVelocity = false;
    ball.motion.keys = { fluid::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                         fluid::FluidMotionKey{ 1.0f, { 0.5f, 0.0f, 0.0f } } };
    recipe.colliders.push_back(ball);
    fluid::FluidCollider box;
    box.shape = fluid::FluidColliderShape::Box;
    box.size = { 0.2f, 0.1f, 0.3f };
    recipe.colliders.push_back(box);
    fluid::FluidCollider ramp;
    ramp.shape = fluid::FluidColliderShape::Plane;
    ramp.center = { 0.0f, -0.5f, 0.0f };
    ramp.direction = { -0.5f, 1.0f, 0.0f };
    recipe.colliders.push_back(ramp);

    const fluid::FluidRecipe loaded = RoundTrip(temp, "colliders.fluid", recipe);
    ASSERT_EQ(loaded.sources.size(), 1u);
    ExpectSourceEq(loaded.sources[0], stamp);
    ASSERT_EQ(loaded.colliders.size(), recipe.colliders.size());
    for (std::size_t i = 0; i < loaded.colliders.size(); ++i) ExpectColliderEq(loaded.colliders[i], recipe.colliders[i]);

    /// @note 形は名前で書く (JSON の添字ではない)。
    std::ifstream stream(temp.File("colliders.fluid"), std::ios::binary);
    std::stringstream text;
    text << stream.rdbuf();
    toml::parse_result parsed = toml::parse(text.str());
    ASSERT_TRUE(parsed);
    const toml::table& root = parsed.table();
    EXPECT_EQ(root["source"][0]["shape"].value_or(std::string{}), "texture");
    EXPECT_EQ(root["collider"][2]["shape"].value_or(std::string{}), "plane");
}

TEST(FluidRecipeBakeTest, ColliderMissingKeysKeepDefaultsAndUnknownShapeIsSphere)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    const fluid::FluidRecipe loaded = LoadText(temp, "collider.fluid",
                                               "version = 2\n"
                                               "kind = \"liquid\"\n"
                                               "\n[[collider]]\n"
                                               /// @note 知らない名前 (将来の形) は既定の sphere に落ちる。
                                               "shape = \"torus\"\n"
                                               "friction = 1.25\n"
                                               "\n[[collider.motion.key]]\ntime = 0.5\noffset = [0.0, 0.2, 0.0]\n"
                                               "\n[[collider.motion.key]]\ntime = 0.1\noffset = [0.0, 0.1, 0.0]\n");
    ASSERT_EQ(loaded.colliders.size(), 1u);
    const fluid::FluidCollider& collider = loaded.colliders[0];
    const fluid::FluidCollider defaults;
    EXPECT_EQ(collider.shape, fluid::FluidColliderShape::Sphere);
    EXPECT_FLOAT_EQ(collider.friction, 1.25f);
    EXPECT_TRUE(collider.enabled);
    ExpectVector3Eq(collider.center, defaults.center);
    ExpectVector3Eq(collider.size, defaults.size);
    ExpectVector3Eq(collider.direction, defaults.direction);
    ASSERT_EQ(collider.motion.keys.size(), 2u);
    EXPECT_FLOAT_EQ(collider.motion.keys[0].time, 0.1f);
}

TEST(FluidRecipeBakeTest, SaveWritesVersion2WithoutLegacyLists)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    const std::filesystem::path file = temp.File("v2.fluid");
    ASSERT_TRUE(asset::SaveFluidRecipe(util::FileSystem::PathToUtf8(file),
                                       asset::MakeFluidPreset(asset::FluidPreset::WaterSplash)));
    std::ifstream stream(file, std::ios::binary);
    std::stringstream text;
    text << stream.rdbuf();
    toml::parse_result parsed = toml::parse(text.str());
    ASSERT_TRUE(parsed);
    const toml::table& root = parsed.table();
    EXPECT_EQ(root["version"].value_or(int64_t{ 0 }), 2);
    EXPECT_TRUE(root["source"].is_array());
    EXPECT_FALSE(static_cast<bool>(root["gas_source"]));
    EXPECT_FALSE(static_cast<bool>(root["liquid_emitter"]));
}

TEST(FluidRecipeBakeTest, Version1GasSourcesMigrateToSources)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    /// @note 使われていなかった側 (気体のレシピの liquid_emitter) は移さない。
    const fluid::FluidRecipe loaded = LoadText(temp, "v1gas.fluid",
                                               "version = 1\n"
                                               "kind = \"gas\"\n"
                                               "\n[[gas_source]]\n"
                                               "shape = \"box\"\n"
                                               "center = [0.1, -0.5, 0.0]\n"
                                               "size = [0.2, 0.3, 0.4]\n"
                                               "density = 3.0\n"
                                               "temperature = 1.5\n"
                                               "fuel = 2.0\n"
                                               "velocity = [0.0, 0.75, 0.0]\n"
                                               "start_time = 0.25\n"
                                               "duration = 0.5\n"
                                               "noise = 0.3\n"
                                               "\n[[gas_source]]\n"
                                               "center = [0.0, 0.0, 0.0]\n"
                                               "\n[[liquid_emitter]]\n"
                                               "count = 10\n");
    EXPECT_EQ(loaded.kind, fluid::FluidKind::Gas);
    ASSERT_EQ(loaded.sources.size(), 2u);
    EXPECT_TRUE(loaded.forces.empty());

    fluid::FluidSource expected;
    expected.shape = fluid::FluidSourceShape::Box;
    expected.center = { 0.1f, -0.5f, 0.0f };
    expected.size = { 0.2f, 0.3f, 0.4f };
    expected.density = 3.0f;
    expected.temperature = 1.5f;
    expected.fuel = 2.0f;
    expected.velocity = { 0.0f, 0.75f, 0.0f };
    expected.startTime = 0.25f;
    expected.duration = 0.5f;
    expected.noise = 0.3f;
    ExpectSourceEq(loaded.sources[0], expected);

    fluid::FluidSource second;
    second.center = { 0.0f, 0.0f, 0.0f };
    ExpectSourceEq(loaded.sources[1], second);
}

TEST(FluidRecipeBakeTest, Version1LiquidEmitterKeepsItsOldDefaults)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    /// @note version の無いファイルは version 1。欠けたキーは当時の液体の既定で埋まる (今の FluidSource の既定ではない)。
    const fluid::FluidRecipe loaded = LoadText(temp, "v1liquid.fluid",
                                               "kind = \"liquid\"\n"
                                               "\n[[liquid_emitter]]\n"
                                               "count = 42\n"
                                               "\n[[gas_source]]\n"
                                               "density = 9.0\n");
    EXPECT_EQ(loaded.kind, fluid::FluidKind::Liquid);
    ASSERT_EQ(loaded.sources.size(), 1u);
    const fluid::FluidSource& source = loaded.sources[0];
    EXPECT_EQ(source.count, 42);
    EXPECT_EQ(source.shape, fluid::FluidSourceShape::Sphere);
    ExpectVector3Eq(source.center, { 0.0f, -0.6f, 0.0f });
    ExpectVector3Eq(source.size, { 0.08f, 0.08f, 0.08f });
    ExpectVector3Eq(source.velocity, { 0.0f, 2.5f, 0.0f });
    EXPECT_FLOAT_EQ(source.spread, 0.5f);
    EXPECT_FLOAT_EQ(source.startTime, 0.0f);
    EXPECT_FLOAT_EQ(source.duration, 0.15f);
    EXPECT_TRUE(source.enabled);
    EXPECT_TRUE(source.motion.keys.empty());

    /// @note 書かれていたキーはそのまま移る。
    const fluid::FluidRecipe full = LoadText(temp, "v1liquid_full.fluid",
                                             "version = 1\n"
                                             "kind = \"liquid\"\n"
                                             "\n[[liquid_emitter]]\n"
                                             "position = [0.2, -0.4, 0.0]\n"
                                             "radius = 0.05\n"
                                             "velocity = [1.0, 2.0, 0.0]\n"
                                             "spread = 0.25\n"
                                             "count = 300\n"
                                             "start_time = 0.5\n"
                                             "duration = 0.0\n");
    ASSERT_EQ(full.sources.size(), 1u);
    ExpectVector3Eq(full.sources[0].center, { 0.2f, -0.4f, 0.0f });
    ExpectVector3Eq(full.sources[0].size, { 0.05f, 0.05f, 0.05f });
    ExpectVector3Eq(full.sources[0].velocity, { 1.0f, 2.0f, 0.0f });
    EXPECT_FLOAT_EQ(full.sources[0].spread, 0.25f);
    EXPECT_EQ(full.sources[0].count, 300);
    EXPECT_FLOAT_EQ(full.sources[0].startTime, 0.5f);
    EXPECT_FLOAT_EQ(full.sources[0].duration, 0.0f);
}

TEST(FluidRecipeBakeTest, ListsAreClampedToTheLimitsOnLoad)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    std::string text = "version = 2\nkind = \"gas\"\n";
    /// @note 先頭の発生源にだけ上限を超えるキーを持たせる。
    text += "\n[[source]]\nname = \"keys\"\n";
    for (int i = 0; i < fluid::kMaxFluidMotionKeys + 4; ++i)
        text += "\n[[source.motion.key]]\ntime = " + std::to_string(i) + ".0\n";
    for (int i = 1; i < fluid::kMaxFluidSources + 4; ++i)
        text += "\n[[source]]\ndensity = " + std::to_string(i) + ".0\n";
    for (int i = 0; i < fluid::kMaxFluidForces + 3; ++i)
        text += "\n[[force]]\nstrength = " + std::to_string(i) + ".0\n";
    for (int i = 0; i < fluid::kMaxFluidColliders + 3; ++i)
        text += "\n[[collider]]\nfriction = " + std::to_string(i) + ".0\n";

    const fluid::FluidRecipe loaded = LoadText(temp, "many.fluid", text);
    ASSERT_EQ(loaded.sources.size(), static_cast<std::size_t>(fluid::kMaxFluidSources));
    ASSERT_EQ(loaded.forces.size(), static_cast<std::size_t>(fluid::kMaxFluidForces));
    ASSERT_EQ(loaded.colliders.size(), static_cast<std::size_t>(fluid::kMaxFluidColliders));
    EXPECT_FLOAT_EQ(loaded.colliders.back().friction, static_cast<float>(fluid::kMaxFluidColliders - 1));
    /// @note ファイルの先頭から上限までが残る。
    EXPECT_EQ(loaded.sources[0].name, "keys");
    EXPECT_FLOAT_EQ(loaded.sources.back().density, static_cast<float>(fluid::kMaxFluidSources - 1));
    EXPECT_FLOAT_EQ(loaded.forces.back().strength, static_cast<float>(fluid::kMaxFluidForces - 1));
    const std::vector<fluid::FluidMotionKey>& keys = loaded.sources[0].motion.keys;
    ASSERT_EQ(keys.size(), static_cast<std::size_t>(fluid::kMaxFluidMotionKeys));
    EXPECT_FLOAT_EQ(keys.back().time, static_cast<float>(fluid::kMaxFluidMotionKeys - 1));
}

TEST(FluidRecipeBakeTest, MotionKeysAreSortedByTimeOnLoad)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    const fluid::FluidRecipe loaded = LoadText(temp, "unsorted.fluid",
                                               "version = 2\n"
                                               "kind = \"gas\"\n"
                                               "\n[[force]]\n"
                                               "type = \"attract\"\n"
                                               "\n[[force.motion.key]]\ntime = 0.5\noffset = [0.5, 0.0, 0.0]\n"
                                               "\n[[force.motion.key]]\ntime = 0.1\noffset = [0.1, 0.0, 0.0]\n"
                                               "\n[[force.motion.key]]\ntime = 0.3\noffset = [0.3, 0.0, 0.0]\n");
    ASSERT_EQ(loaded.forces.size(), 1u);
    /// @note [[collider]] より前の version 2 のファイルは障害物なしで開ける。
    EXPECT_TRUE(loaded.colliders.empty());
    EXPECT_EQ(loaded.forces[0].type, fluid::FluidForceType::Attract);
    const std::vector<fluid::FluidMotionKey>& keys = loaded.forces[0].motion.keys;
    ASSERT_EQ(keys.size(), 3u);
    const float times[] = { 0.1f, 0.3f, 0.5f };
    for (std::size_t i = 0; i < keys.size(); ++i) {
        EXPECT_FLOAT_EQ(keys[i].time, times[i]);
        /// @note offset はキーと一緒に並び替わる
        EXPECT_FLOAT_EQ(keys[i].offset.x, times[i]);
    }
}

TEST(FluidRecipeBakeTest, EveryPresetRoundTripsAndHasAnEnabledSource)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i) {
        const auto preset = static_cast<asset::FluidPreset>(i);
        SCOPED_TRACE(asset::FluidPresetName(preset));
        const fluid::FluidRecipe recipe = asset::MakeFluidPreset(preset);
        EXPECT_TRUE(std::any_of(recipe.sources.begin(), recipe.sources.end(),
                                [](const fluid::FluidSource& source) { return source.enabled; }));
        EXPECT_LE(recipe.sources.size(), static_cast<std::size_t>(fluid::kMaxFluidSources));
        EXPECT_LE(recipe.forces.size(), static_cast<std::size_t>(fluid::kMaxFluidForces));
        for (const fluid::FluidSource& source : recipe.sources) EXPECT_FALSE(source.name.empty());
        for (const fluid::FluidForce& force : recipe.forces) EXPECT_FALSE(force.name.empty());
        EXPECT_LE(recipe.colliders.size(), static_cast<std::size_t>(fluid::kMaxFluidColliders));
        for (const fluid::FluidCollider& collider : recipe.colliders) EXPECT_FALSE(collider.name.empty());
        /// @note 同梱の画像が無いので、Texture の発生源を使うプリセットは作らない (焼くと板の形がそのまま出る)。
        for (const fluid::FluidSource& source : recipe.sources)
            EXPECT_NE(source.shape, fluid::FluidSourceShape::Texture);

        const fluid::FluidRecipe loaded = RoundTrip(temp, "preset.fluid", recipe);
        EXPECT_EQ(loaded.kind, recipe.kind);
        EXPECT_EQ(loaded.seed, recipe.seed);
        EXPECT_EQ(loaded.render.shading, recipe.render.shading);
        EXPECT_EQ(loaded.output.loop, recipe.output.loop);
        ExpectBakeEq(loaded.bake, recipe.bake);
        ASSERT_EQ(loaded.sources.size(), recipe.sources.size());
        for (std::size_t s = 0; s < loaded.sources.size(); ++s) ExpectSourceEq(loaded.sources[s], recipe.sources[s]);
        ASSERT_EQ(loaded.forces.size(), recipe.forces.size());
        for (std::size_t f = 0; f < loaded.forces.size(); ++f) ExpectForceEq(loaded.forces[f], recipe.forces[f]);
        ASSERT_EQ(loaded.colliders.size(), recipe.colliders.size());
        for (std::size_t c = 0; c < loaded.colliders.size(); ++c)
            ExpectColliderEq(loaded.colliders[c], recipe.colliders[c]);
        ExpectRampEq(loaded.render.emissionRamp, recipe.render.emissionRamp);
        EXPECT_EQ(loaded.render.useAlbedoRamp, recipe.render.useAlbedoRamp);
        ExpectRampEq(loaded.render.albedoRamp, recipe.render.albedoRamp);
    }
}

TEST(FluidRecipeBakeTest, PresetsAreBuiltFromTheSameParts)
{
    /// @note 液体のプリセットは旧 emitter の値を FluidSource へ移している。先頭の emitter が
    /// @note «球・速度・ばらつき・数・射出時間» を持つことだけを見る。数値を固定すると
    /// @note プリセットの絵を詰め直せなくなるため、値そのものは縛らない。
    const fluid::FluidRecipe splash = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    ASSERT_GE(splash.sources.size(), 1u);
    EXPECT_EQ(splash.sources[0].shape, fluid::FluidSourceShape::Sphere);
    EXPECT_GT(splash.sources[0].size.x, 0.0f);
    EXPECT_GT(splash.sources[0].velocity.y, 0.0f);
    EXPECT_GT(splash.sources[0].spread, 0.0f);
    EXPECT_GT(splash.sources[0].count, 0);
    EXPECT_GT(splash.sources[0].duration, 0.0f);

    const fluid::FluidRecipe steam = asset::MakeFluidPreset(asset::FluidPreset::Steam);
    ASSERT_FALSE(steam.sources.empty());
    EXPECT_EQ(steam.sources[0].shape, fluid::FluidSourceShape::Cone);

    const fluid::FluidRecipe wisp = asset::MakeFluidPreset(asset::FluidPreset::MagicWisp);
    ASSERT_GE(wisp.forces.size(), 1u);
    EXPECT_EQ(wisp.forces[0].type, fluid::FluidForceType::Vortex);

    const fluid::FluidRecipe ink = asset::MakeFluidPreset(asset::FluidPreset::Ink);
    ASSERT_FALSE(ink.sources.empty());
    EXPECT_GE(ink.sources[0].motion.keys.size(), 2u);

    /// @note 発生源ごとの色の見本は爆発: 芯と外側の土煙で色の鍵が違い、albedo_ramp を使う。
    const fluid::FluidRecipe boom = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    EXPECT_TRUE(boom.render.useAlbedoRamp);
    ASSERT_GE(boom.sources.size(), 2u);
    EXPECT_NE(boom.sources[0].colorKey, boom.sources[1].colorKey);
    for (std::size_t i = 1; i < boom.render.albedoRamp.stops.size(); ++i)
        EXPECT_LE(boom.render.albedoRamp.stops[i - 1].position, boom.render.albedoRamp.stops[i].position);

    /// @note 障害物の見本は噴流の岩 1 つだけ (他のプリセットの絵は変えない)。岩は床に接地している。
    int withColliders = 0;
    for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i)
        if (!asset::MakeFluidPreset(static_cast<asset::FluidPreset>(i)).colliders.empty()) ++withColliders;
    EXPECT_EQ(withColliders, 1);
    const fluid::FluidRecipe jet = asset::MakeFluidPreset(asset::FluidPreset::WaterJet);
    ASSERT_EQ(jet.colliders.size(), 1u);
    EXPECT_EQ(jet.colliders[0].shape, fluid::FluidColliderShape::Box);
    EXPECT_NEAR(jet.colliders[0].center.y - jet.colliders[0].size.y, jet.liquid.floorHeight, 1.0e-5f);
}

TEST(FluidRecipeBakeTest, VolumeSettingsComeFromOutputAndRender)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.output.columns = 6;
    recipe.output.rows = 5;
    recipe.output.duration = 3.0f;
    recipe.output.frameSize = 128;
    recipe.output.supersampling = 4;
    recipe.render.detailStrength = 0.4f;
    recipe.render.detailScale = 12.0f;
    recipe.gas.detailPeriod = 0.6f;
    recipe.bake = NonDefaultBake();

    const std::string path = util::FileSystem::PathToUtf8(temp.File("MySmoke.fluid"));
    const asset::VolumeFlipbookBakeSettings settings = asset::MakeVolumeBakeSettings(recipe, path);
    EXPECT_EQ(settings.source.frameCount, 30);
    EXPECT_FLOAT_EQ(settings.source.frameDt, 0.1f);
    EXPECT_EQ(settings.tileSize, 128);
    EXPECT_EQ(settings.columns, 6);
    /// @note Volume Baker の上限
    EXPECT_EQ(settings.supersampling, 3);
    EXPECT_FLOAT_EQ(settings.detailStrength, 0.4f);
    EXPECT_FLOAT_EQ(settings.detailScale, 12.0f);
    EXPECT_FLOAT_EQ(settings.detailPeriod, 0.6f);
    EXPECT_EQ(settings.sourceKind, asset::VolumeSourceKind::Fluid);
    EXPECT_EQ(settings.fluidRecipePath, path);
    EXPECT_EQ(settings.fluidSolver, asset::VolumeFluidSolver::Cpu);
    EXPECT_FLOAT_EQ(settings.fluidDensityScale, 2.5f);
    EXPECT_FLOAT_EQ(settings.extinction, recipe.bake.extinction);
    EXPECT_EQ(settings.volumeResolution, 96);
    EXPECT_FLOAT_EQ(settings.exposure, 1.3f);
    EXPECT_FLOAT_EQ(settings.halfExtent, 1.4f);
    EXPECT_TRUE(settings.blackbodyEmission);
    /// @note path は gtest の表示でコンテナ扱いされるので、比較だけして文字列で報告する。
    EXPECT_TRUE(util::FileSystem::PathFromUtf8(settings.outputDirectory).lexically_normal()
                == temp.Path().lexically_normal())
        << settings.outputDirectory;
    EXPECT_EQ(settings.baseName, "MySmoke");

    /// @note 煙は温度を持つが光らせない。炎は炎の Ramp。
    const math::Vector3 hottestSmoke = asset::EvaluateVolumeRamp(settings.emissionRamp, 1.0f);
    EXPECT_FLOAT_EQ(hottestSmoke.x + hottestSmoke.y + hottestSmoke.z, 0.0f);
    recipe.render.shading = fluid::FluidShading::Fire;
    const math::Vector3 hottestFire =
        asset::EvaluateVolumeRamp(asset::MakeVolumeBakeSettings(recipe, path).emissionRamp, 1.0f);
    ExpectVector3Eq(hottestFire, asset::EvaluateVolumeRamp(asset::DefaultFireRamp(), 1.0f));

    /// @note コマ数は Volume Baker の範囲 [2, 256] に丸めてから間隔を割る。
    recipe.output.columns = 32;
    recipe.output.rows = 32;
    const asset::VolumeFlipbookBakeSettings many = asset::MakeVolumeBakeSettings(recipe, path);
    EXPECT_EQ(many.source.frameCount, 256);
    EXPECT_FLOAT_EQ(many.source.frameDt, 3.0f / 256.0f);
    recipe.output.columns = 1;
    recipe.output.rows = 1;
    EXPECT_EQ(asset::MakeVolumeBakeSettings(recipe, path).source.frameCount, 2);
}

TEST(FluidRecipeBakeTest, LoopBlendRoundTripsAndReachesVolumeBaker)
{
    testkit::TempDir temp{ "fluidloopblend" };
    ASSERT_TRUE(temp.IsValid());
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    recipe.output.loop = true;
    recipe.output.loopBlendFraction = 0.125f;
    const fluid::FluidRecipe loaded = RoundTrip(temp, "fire.fluid", recipe);
    EXPECT_FLOAT_EQ(loaded.output.loopBlendFraction, 0.125f);
    const asset::VolumeFlipbookBakeSettings settings =
        asset::MakeVolumeBakeSettings(loaded, "C:/Fx/Fire.fluid");
    EXPECT_FLOAT_EQ(settings.fluidLoopBlendFraction, 0.125f);
    EXPECT_EQ(asset::VolumeLoopOverlapFrames(settings), fluid::MakeFluidStepPlan(loaded).loopOverlap);
}

TEST(FluidRecipeBakeTest, RenderOpacityScalesVolumeExtinctionAndStoreReversesIt)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.bake.extinction = 12.0f;
    const std::string path = "C:/Fx/Opacity.fluid";
    const float referenceOpacity = fluid::FluidRenderSettings{}.opacity;

    recipe.render.opacity = referenceOpacity * 0.5f;
    asset::VolumeFlipbookBakeSettings settings = asset::MakeVolumeBakeSettings(recipe, path);
    EXPECT_FLOAT_EQ(settings.extinction, recipe.bake.extinction * 0.5f);

    settings.extinction = 9.0f;
    asset::StoreVolumeBakeSettings(settings, recipe);
    EXPECT_FLOAT_EQ(recipe.bake.extinction, 18.0f);
    EXPECT_FLOAT_EQ(asset::MakeVolumeBakeSettings(recipe, path).extinction, 9.0f);
}

TEST(FluidRecipeBakeTest, FireMaskUsesBakeExtinctionIndependentOfRenderOpacity)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    recipe.bake.extinction = 12.0f;
    const float referenceOpacity = fluid::FluidRenderSettings{}.opacity;
    recipe.render.opacity = referenceOpacity;

    const asset::VolumeFlipbookBakeSettings defaultOpacity =
        asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Fire.fluid");
    EXPECT_FLOAT_EQ(defaultOpacity.extinction, 12.0f);
    EXPECT_FLOAT_EQ(defaultOpacity.fireEmissionExtinction, 12.0f);

    recipe.render.opacity = referenceOpacity * 0.5f;
    const asset::VolumeFlipbookBakeSettings lowOpacity =
        asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Fire.fluid");
    EXPECT_TRUE(lowOpacity.fireEmission);
    EXPECT_FLOAT_EQ(lowOpacity.extinction, 6.0f);
    EXPECT_FLOAT_EQ(lowOpacity.fireEmissionExtinction, 12.0f);

    recipe.render.opacity = referenceOpacity * 2.0f;
    const asset::VolumeFlipbookBakeSettings highOpacity =
        asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Fire.fluid");
    EXPECT_FLOAT_EQ(highOpacity.extinction, 24.0f);
    EXPECT_FLOAT_EQ(highOpacity.fireEmissionExtinction, 12.0f);

    recipe.render.shading = fluid::FluidShading::Smoke;
    EXPECT_FALSE(asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Smoke.fluid").fireEmission);
}

TEST(FluidRecipeBakeTest, FireKelvinMatchesNormalizedVolumeTemperature)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    fluid::FluidSource source;
    source.temperature = 1.6f;
    recipe.sources = { source };
    recipe.render.fireKelvin = 1500.0f;
    recipe.bake.blackbodyMaxKelvin = 2400.0f;

    const float temperatureScale = asset::FluidRecipeTemperatureScale(recipe);
    const asset::VolumeFlipbookBakeSettings settings =
        asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Fire.fluid");
    const float volumeKelvin = settings.blackbodyMaxKelvin * source.temperature * temperatureScale;
    const float twoDKelvin = recipe.render.fireKelvin * source.temperature;

    EXPECT_FLOAT_EQ(temperatureScale * source.temperature, 1.0f);
    EXPECT_FLOAT_EQ(volumeKelvin, twoDKelvin);
    EXPECT_FLOAT_EQ(settings.blackbodyLutMaxKelvin, recipe.render.fireKelvin * 4.0f);
}

TEST(FluidRecipeBakeTest, FireRenderControlsPreserveVolumeCalibration)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Fire);
    recipe.bake.blackbodyEmission = false;
    recipe.bake.blackbodyMaxKelvin = 3500.0f;
    recipe.bake.emissionIntensity = 36.0f;
    recipe.render.fireKelvin = 1500.0f;
    recipe.render.fireIntensity = 1.0f;
    recipe.render.useEmissionRamp = false;

    const asset::VolumeFlipbookBakeSettings calibrated =
        asset::MakeVolumeBakeSettings(recipe, "C:/Fx/JetFlame.fluid");
    EXPECT_TRUE(calibrated.fireEmission);
    EXPECT_TRUE(calibrated.blackbodyEmission);
    EXPECT_FLOAT_EQ(calibrated.blackbodyMaxKelvin, 3500.0f);
    EXPECT_FLOAT_EQ(calibrated.blackbodyLutMaxKelvin, 6000.0f);
    EXPECT_FLOAT_EQ(calibrated.emissionIntensity, 36.0f);

    recipe.render.fireIntensity = 0.5f;
    const asset::VolumeFlipbookBakeSettings dimmed =
        asset::MakeVolumeBakeSettings(recipe, "C:/Fx/JetFlame.fluid");
    EXPECT_FLOAT_EQ(dimmed.emissionIntensity, 18.0f);
    asset::StoreVolumeBakeSettings(dimmed, recipe);
    EXPECT_FLOAT_EQ(recipe.bake.emissionIntensity, 36.0f);
    EXPECT_FALSE(recipe.bake.blackbodyEmission);
    EXPECT_FLOAT_EQ(recipe.bake.blackbodyMaxKelvin, 3500.0f);

    recipe.render.useEmissionRamp = true;
    recipe.render.emissionRamp.stops[3] = { { 2.0f, 0.5f, 0.1f }, 1.0f };
    const asset::VolumeFlipbookBakeSettings ramped =
        asset::MakeVolumeBakeSettings(recipe, "C:/Fx/JetFlame.fluid");
    EXPECT_FALSE(ramped.blackbodyEmission);
    ExpectVector3Eq(asset::EvaluateVolumeRamp(ramped.emissionRamp, 1.0f), { 2.0f, 0.5f, 0.1f });
}

TEST(FluidRecipeBakeTest, FireRadianceUsesSharedColorAndUnitBoxPathIntegral)
{
    constexpr float referenceKelvin = 1500.0f;
    constexpr float temperature = 1.6f;
    constexpr float intensity = 1.0f;
    const asset::FluidFireColorLut lut(referenceKelvin * 4.0f);
    const math::Vector3 chroma = lut.Chroma(referenceKelvin * temperature);
    const math::Vector3 directChroma = scene::ParticleBlackbodyChroma(referenceKelvin * temperature);
    EXPECT_NEAR(chroma.x, directChroma.x, 0.01f);
    EXPECT_NEAR(chroma.y, directChroma.y, 0.01f);
    EXPECT_NEAR(chroma.z, directChroma.z, 0.01f);

    const math::Vector3 planarRadiance = asset::FluidFireBlackbodyRadiance(temperature, intensity, chroma);
    math::Vector3 volumeRadiance = math::Vector3::ZERO;
    constexpr int segmentCount = 128;
    constexpr float segmentLength = asset::kFluidFireReferencePathLength / static_cast<float>(segmentCount);
    for (int i = 0; i < segmentCount; ++i)
        volumeRadiance = volumeRadiance + asset::FluidFireSegmentContribution(planarRadiance, 1.0f, 0.0f,
                                                                              segmentLength);

    const math::Vector3 planarFire = asset::FluidFireSoftKnee(planarRadiance);
    const math::Vector3 volumeFire = asset::FluidFireSoftKnee(volumeRadiance);
    EXPECT_NEAR(volumeFire.x, planarFire.x, 1.0e-5f);
    EXPECT_NEAR(volumeFire.y, planarFire.y, 1.0e-5f);
    EXPECT_NEAR(volumeFire.z, planarFire.z, 1.0e-5f);
    EXPECT_NEAR(asset::FluidFireMeanTransmittance(1.0f, 2.0f, 1.0f),
                (1.0f - std::exp(-2.0f)) / 2.0f, 1.0e-6f);

    const math::Vector3 rampColor{ 0.2f, 0.4f, 0.8f };
    const math::Vector3 rampRadiance = asset::FluidFireRampRadiance(rampColor, 3.0f);
    EXPECT_FLOAT_EQ(rampRadiance.x, 0.6f);
    EXPECT_FLOAT_EQ(rampRadiance.y, 1.2f);
    EXPECT_FLOAT_EQ(rampRadiance.z, 2.4f);
}

TEST(FluidRecipeBakeTest, UserEmissionRampReachesTheVolumeBaker)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.render.useEmissionRamp = true;
    recipe.render.emissionRamp.stops[2] = { { 0.1f, 0.8f, 2.0f }, 0.5f };
    recipe.render.emissionRamp.stops[3] = { { 7.0f, 1.0f, 0.5f }, 1.0f };

    /// @note 煙でも Ramp を指定すればその色で光る (shading によらない)。リニアのまま写す。
    const asset::VolumeFlipbookBakeSettings settings = asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Tinted.fluid");
    for (std::size_t i = 0; i < settings.emissionRamp.stops.size(); ++i) {
        ExpectVector3Eq(settings.emissionRamp.stops[i].color, recipe.render.emissionRamp.stops[i].color);
        EXPECT_FLOAT_EQ(settings.emissionRamp.stops[i].position, recipe.render.emissionRamp.stops[i].position);
    }
    recipe.render.shading = fluid::FluidShading::Fire;
    ExpectVector3Eq(asset::EvaluateVolumeRamp(asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Tinted.fluid").emissionRamp,
                                              1.0f),
                    { 7.0f, 1.0f, 0.5f });
}

TEST(FluidRecipeBakeTest, LiquidAlbedoIsTheLiquidColor)
{
    const fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    const asset::VolumeFlipbookBakeSettings settings = asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Splash.fluid");
    const math::Vector4& color = recipe.render.liquidColor;
    for (const asset::VolumeRampStop& stop : settings.albedoRamp.stops) {
        EXPECT_NEAR(stop.color.x, ToLinear(color.x), 1.0e-5f);
        EXPECT_NEAR(stop.color.y, ToLinear(color.y), 1.0e-5f);
        EXPECT_NEAR(stop.color.z, ToLinear(color.z), 1.0e-5f);
    }
    EXPECT_FLOAT_EQ(settings.liquid.specular, recipe.render.specular);
    EXPECT_EQ(settings.fluidSolver, asset::VolumeFluidSolver::Cpu);
    EXPECT_EQ(settings.baseName, "Splash");
}

TEST(FluidRecipeBakeTest, LiquidSurfaceSettingsReachTheVolumeBaker)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    recipe.render.liquidSoftness = 0.2f;
    recipe.render.liquidExtinction = 33.0f;
    recipe.render.liquidGloss = 50.0f;
    recipe.render.liquidFresnel = 0.05f;
    const asset::VolumeFlipbookBakeSettings settings = asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Splash.fluid");
    EXPECT_FLOAT_EQ(settings.liquid.softness, 0.2f);
    EXPECT_FLOAT_EQ(settings.liquid.extinction, 33.0f);
    EXPECT_FLOAT_EQ(settings.liquid.gloss, 50.0f);
    EXPECT_FLOAT_EQ(settings.liquid.fresnelF0, 0.05f);
}

TEST(FluidRecipeBakeTest, StoreIsTheInverseOfMake)
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    recipe.bake = NonDefaultBake();
    const std::string path = "C:/Fx/Boom.fluid";

    fluid::FluidRecipe stored = recipe;
    asset::StoreVolumeBakeSettings(asset::MakeVolumeBakeSettings(recipe, path), stored);
    ExpectBakeEq(stored.bake, recipe.bake);

    /// @note パネルで触った値は [bake] へ戻り、[output] (コマ数・タイル) は触らない。
    asset::VolumeFlipbookBakeSettings edited = asset::MakeVolumeBakeSettings(recipe, path);
    edited.exposure = 2.0f;
    edited.fluidSolver = asset::VolumeFluidSolver::Gpu;
    edited.tileSize = 512;
    edited.source.frameCount = 12;
    asset::StoreVolumeBakeSettings(edited, stored);
    EXPECT_FLOAT_EQ(stored.bake.exposure, 2.0f);
    EXPECT_EQ(stored.bake.solver, fluid::FluidBakeSolver::Gpu);
    EXPECT_EQ(stored.bake.mode, recipe.bake.mode);
    EXPECT_EQ(stored.output.frameSize, recipe.output.frameSize);
    EXPECT_EQ(stored.output.columns, recipe.output.columns);
    EXPECT_EQ(stored.output.rows, recipe.output.rows);
}

TEST(FluidRecipeBakeTest, AlbedoRampIsSortedOnLoadAndOldFilesKeepItOff)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    /// @note albedo_ramp / color_key の無い古いファイルは 1 色のまま (鍵 0)。
    const fluid::FluidRecipe old = LoadText(temp, "old_albedo.fluid",
                                            "version = 2\nkind = \"gas\"\n\n[[source]]\nname = \"a\"\n");
    EXPECT_FALSE(old.render.useAlbedoRamp);
    ExpectRampEq(old.render.albedoRamp, fluid::FluidRenderSettings{}.albedoRamp);
    ASSERT_EQ(old.sources.size(), 1u);
    EXPECT_FLOAT_EQ(old.sources[0].colorKey, 0.0f);

    const fluid::FluidRecipe loaded = LoadText(temp, "unsorted_albedo.fluid",
                                               "version = 2\n"
                                               "kind = \"gas\"\n"
                                               "\n[render]\nuse_albedo_ramp = true\n"
                                               "\n[[render.albedo_ramp]]\ncolor = [1.0, 0.0, 0.0]\nposition = 1.0\n"
                                               "\n[[render.albedo_ramp]]\ncolor = [0.0, 1.0, 0.0]\nposition = 0.0\n"
                                               "\n[[render.albedo_ramp]]\ncolor = [0.0, 0.0, 1.0]\nposition = 0.66\n"
                                               "\n[[render.albedo_ramp]]\ncolor = [1.0, 1.0, 0.0]\nposition = 0.33\n"
                                               "\n[[source]]\ncolor_key = 0.4\n");
    EXPECT_TRUE(loaded.render.useAlbedoRamp);
    const auto& stops = loaded.render.albedoRamp.stops;
    const float positions[] = { 0.0f, 0.33f, 0.66f, 1.0f };
    for (std::size_t i = 0; i < stops.size(); ++i) EXPECT_FLOAT_EQ(stops[i].position, positions[i]);
    /// @note 色は点と一緒に並び替わる
    ExpectVector3Eq(stops[0].color, { 0.0f, 1.0f, 0.0f });
    ExpectVector3Eq(stops[3].color, { 1.0f, 0.0f, 0.0f });
    ASSERT_EQ(loaded.sources.size(), 1u);
    EXPECT_FLOAT_EQ(loaded.sources[0].colorKey, 0.4f);
}

TEST(FluidRecipeBakeTest, VolumeSettingsCarryLoopDistortionAndAlbedoRamp)
{
    const auto expectRamp = [](const asset::VolumeColorRamp& actual, const fluid::FluidColorRamp& expected) {
        for (std::size_t i = 0; i < actual.stops.size(); ++i) {
            ExpectVector3Eq(actual.stops[i].color, expected.stops[i].color);
            EXPECT_FLOAT_EQ(actual.stops[i].position, expected.stops[i].position);
        }
    };

    /// @note 陽炎: ループし、色の代わりに歪みを焼く。倍率は 2D の符号化 (速さ 1 で変位 0.42) と同じ。
    const fluid::FluidRecipe haze = asset::MakeFluidPreset(asset::FluidPreset::HeatHaze);
    const asset::VolumeFlipbookBakeSettings hazeSettings = asset::MakeVolumeBakeSettings(haze, "C:/Fx/Haze.fluid");
    EXPECT_TRUE(hazeSettings.fluidLoop);
    EXPECT_TRUE(hazeSettings.distortion);
    EXPECT_FLOAT_EQ(hazeSettings.distortionScale, 0.42f);

    /// @note 一度きりの煙はループも歪みもしない。
    const asset::VolumeFlipbookBakeSettings smoke =
        asset::MakeVolumeBakeSettings(asset::MakeFluidPreset(asset::FluidPreset::Smoke), "C:/Fx/Smoke.fluid");
    EXPECT_FALSE(smoke.fluidLoop);
    EXPECT_FALSE(smoke.distortion);
    EXPECT_TRUE(asset::MakeVolumeBakeSettings(asset::MakeFluidPreset(asset::FluidPreset::Fire), "C:/Fx/Fire.fluid")
                    .fluidLoop);

    /// @note 気体: albedo_ramp はリニアのまま写す。切れば smoke_color の 1 色。
    fluid::FluidRecipe boom = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    ASSERT_TRUE(boom.render.useAlbedoRamp);
    expectRamp(asset::MakeVolumeBakeSettings(boom, "C:/Fx/Boom.fluid").albedoRamp, boom.render.albedoRamp);
    boom.render.useAlbedoRamp = false;
    const asset::VolumeFlipbookBakeSettings plain = asset::MakeVolumeBakeSettings(boom, "C:/Fx/Boom.fluid");
    for (const asset::VolumeRampStop& stop : plain.albedoRamp.stops) {
        EXPECT_NEAR(stop.color.x, ToLinear(boom.render.smokeColor.x), 1.0e-5f);
        EXPECT_NEAR(stop.color.y, ToLinear(boom.render.smokeColor.y), 1.0e-5f);
        EXPECT_NEAR(stop.color.z, ToLinear(boom.render.smokeColor.z), 1.0e-5f);
    }

    /// @note 液体: albedo_ramp を使うなら liquid_color の 1 色の代わりに Ramp。
    fluid::FluidRecipe splash = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    splash.render.useAlbedoRamp = true;
    splash.render.albedoRamp.stops[0] = { { 0.9f, 0.1f, 0.1f }, 0.0f };
    splash.render.albedoRamp.stops[3] = { { 0.1f, 0.2f, 0.9f }, 1.0f };
    expectRamp(asset::MakeVolumeBakeSettings(splash, "C:/Fx/Splash.fluid").albedoRamp, splash.render.albedoRamp);
}

TEST(FluidSourceMaskTest, ValueIsLuminanceTimesAlphaWithTopRowFirst)
{
    testkit::TempDir temp{ "fluidmask" };
    ASSERT_TRUE(temp.IsValid());

    /// @note 左半分は白 (下 1/4 だけ黒)、右半分は透明な白。縦横比は 2:1 (マスクは正方形へ引き伸ばす)。
    constexpr int kWidth = 64;
    constexpr int kHeight = 32;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* pixel = &rgba[(static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)) * 4];
            const bool left = x < kWidth / 2;
            const std::uint8_t level = (left && y >= kHeight * 3 / 4) ? 0 : 255;
            pixel[0] = level;
            pixel[1] = level;
            pixel[2] = level;
            pixel[3] = left ? 255 : 0;
        }
    }
    const std::filesystem::path file = temp.File("mask.tga");
    ASSERT_TRUE(WriteTgaRgba8(file, kWidth, kHeight, rgba));

    fluid::FluidSourceMask mask;
    std::string error;
    ASSERT_TRUE(asset::LoadFluidSourceMask(util::FileSystem::PathToUtf8(file), mask, &error)) << error;
    ASSERT_TRUE(mask.IsValid());
    /// @note 白・不透明
    EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, 0.1f, 0.1f), 1.0f, 1.0e-3f);
    /// @note 透明
    EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, 0.9f, 0.5f), 0.0f, 1.0e-3f);
    /// @note 下の黒 (v は上が 0)
    EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, 0.1f, 0.95f), 0.0f, 1.0e-3f);
    /// @note 範囲外は縁の値。
    EXPECT_FLOAT_EQ(fluid::SampleFluidSourceMask(mask, -3.0f, 0.1f), fluid::SampleFluidSourceMask(mask, 0.0f, 0.1f));
    EXPECT_FLOAT_EQ(fluid::SampleFluidSourceMask(mask, 0.1f, 5.0f), fluid::SampleFluidSourceMask(mask, 0.1f, 1.0f));
    EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, 0.0f, 0.0f), 1.0f, 1.0e-3f);
    for (const float value : mask.values) {
        ASSERT_GE(value, 0.0f);
        ASSERT_LE(value, 1.0f);
    }
}

TEST(FluidSourceMaskTest, ShrinkingKeepsThinLines)
{
    testkit::TempDir temp{ "fluidmask" };
    ASSERT_TRUE(temp.IsValid());

    /// @note 4 列に 1 本の白線を 1/4 に縮める。点で拾う双線形だと線の間だけを拾って 0 になる。
    constexpr int kWidth = 1024;
    constexpr int kHeight = 4;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* pixel = &rgba[(static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)) * 4];
            const std::uint8_t level = (x % 4 == 0) ? 255 : 0;
            pixel[0] = level;
            pixel[1] = level;
            pixel[2] = level;
            pixel[3] = 255;
        }
    }
    const std::filesystem::path file = temp.File("lines.tga");
    ASSERT_TRUE(WriteTgaRgba8(file, kWidth, kHeight, rgba));

    fluid::FluidSourceMask mask;
    ASSERT_TRUE(asset::LoadFluidSourceMask(util::FileSystem::PathToUtf8(file), mask));
    for (const float u : { 0.1f, 0.37f, 0.5f, 0.83f })
        EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, u, 0.5f), 0.25f, 1.0e-3f) << u;
}

TEST(FluidSourceMaskTest, MissingOrBrokenFileLeavesTheMaskInvalid)
{
    testkit::TempDir temp{ "fluidmask" };
    ASSERT_TRUE(temp.IsValid());

    fluid::FluidSourceMask mask;
    std::string error;
    EXPECT_FALSE(asset::LoadFluidSourceMask(util::FileSystem::PathToUtf8(temp.File("missing.png")), mask, &error));
    EXPECT_FALSE(mask.IsValid());
    EXPECT_FALSE(error.empty());
    /// @note 読めていないマスクは 1 (板の形のまま湧く)。
    EXPECT_FLOAT_EQ(fluid::SampleFluidSourceMask(mask, 0.3f, 0.7f), 1.0f);

    const std::filesystem::path broken = temp.File("broken.png");
    ASSERT_TRUE(util::FileSystem::WriteText(broken, std::string("not an image")));
    EXPECT_FALSE(asset::LoadFluidSourceMask(util::FileSystem::PathToUtf8(broken), mask));
    EXPECT_FALSE(mask.IsValid());
    EXPECT_FALSE(asset::LoadFluidSourceMask(std::string{}, mask));
}

TEST(FluidSourceMaskTest, SpriteReferenceUsesOnlyThatCell)
{
    testkit::TempDir temp{ "fluidmask" };
    ASSERT_TRUE(temp.IsValid());

    /// @note 4 象限のシート。左上だけ白 (それ以外は黒)。コマを切り抜けていれば «全部 1» と «全部 0» に割れる。
    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* pixel = &rgba[(static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)) * 4];
            const std::uint8_t level = (x < kWidth / 2 && y < kHeight / 2) ? 255 : 0;
            pixel[0] = level;
            pixel[1] = level;
            pixel[2] = level;
            pixel[3] = 255;
        }
    }
    const std::filesystem::path file = temp.File("atlas.tga");
    ASSERT_TRUE(WriteTgaRgba8(file, kWidth, kHeight, rgba));
    ASSERT_TRUE(util::FileSystem::WriteText(
        temp.File("atlas.tga.meta"),
        std::string(R"([texture]
type = "sprite"
sprite_mode = "Multiple"
sprites = [
  { id = "id-lit", name = "Lit", x = 0, y = 0, width = 32, height = 32 },
  { id = "id-dark", name = "Dark", x = 32, y = 32, width = 32, height = 32 },
]
)")));

    const std::string image = util::FileSystem::PathToUtf8(file);
    fluid::FluidSourceMask mask;
    std::string error;

    /// @note ID でも名前でも同じコマを指す。
    for (const char* token : { "id-lit", "Lit" }) {
        ASSERT_TRUE(asset::LoadFluidSourceMask(asset::MakeSpriteReference(image, token), mask, &error)) << error;
        ASSERT_TRUE(mask.IsValid());
        for (const float u : { 0.05f, 0.5f, 0.95f })
            EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, u, u), 1.0f, 1.0e-3f) << token << " " << u;
    }

    ASSERT_TRUE(asset::LoadFluidSourceMask(asset::MakeSpriteReference(image, "Dark"), mask, &error)) << error;
    for (const float u : { 0.05f, 0.5f, 0.95f })
        EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, u, u), 0.0f, 1.0e-3f) << u;

    /// @note 参照のまま (切り抜かずに) 読むとシート全体なので、左上の 1/4 だけが白い。
    ASSERT_TRUE(asset::LoadFluidSourceMask(image, mask, &error)) << error;
    EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, 0.25f, 0.25f), 1.0f, 1.0e-3f);
    EXPECT_NEAR(fluid::SampleFluidSourceMask(mask, 0.75f, 0.75f), 0.0f, 1.0e-3f);

    /// @note 切れた参照はアトラス全面へ落とさず «読めない» にする (呼び手が赤く言える)。
    EXPECT_FALSE(asset::LoadFluidSourceMask(asset::MakeSpriteReference(image, "Gone"), mask, &error));
    EXPECT_FALSE(mask.IsValid());
    EXPECT_FALSE(error.empty());
}

}
