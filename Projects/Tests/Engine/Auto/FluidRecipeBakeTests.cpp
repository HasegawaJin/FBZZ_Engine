/// @file    FluidRecipeBakeTests.cpp
/// @brief   .fluid の [bake]・部品 ([[source]] / [[force]] / [[collider]])・ReflectFluidRecipe の項目名・3D 焼き設定への写し・テクスチャ発生源のマスクを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// AI は ReflectFluidRecipe のフィールド名で .fluid を書き換え、焼くときは TOML を読み直す。
/// 名前が 1 つずれるだけで «書いたのに効かない» になり、エラーはどこにも出ない。

#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Engine/Asset/FluidRecipe.hpp>
#include <Engine/Asset/FluidSourceMask.hpp>
#include <Engine/Asset/FluidVolumeBake.hpp>
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

/// 訪れたフィールドを «gas.buoyancy» «source[].motion.key[].time» の形で集める。Enum は選ばれている名前も集める。
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

asset::FluidBakeSettings NonDefaultBake()
{
    asset::FluidBakeSettings bake;
    bake.mode = asset::FluidBakeMode::Volume3D;
    bake.volumeResolution = 96;
    bake.raySteps = 256;
    bake.shadowSteps = 24;
    bake.solver = asset::FluidBakeSolver::Cpu;
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

void ExpectBakeEq(const asset::FluidBakeSettings& actual, const asset::FluidBakeSettings& expected)
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

void ExpectMotionEq(const asset::FluidMotion& actual, const asset::FluidMotion& expected)
{
    EXPECT_EQ(actual.inheritVelocity, expected.inheritVelocity);
    ASSERT_EQ(actual.keys.size(), expected.keys.size());
    for (std::size_t i = 0; i < actual.keys.size(); ++i) {
        EXPECT_FLOAT_EQ(actual.keys[i].time, expected.keys[i].time);
        ExpectVector3Eq(actual.keys[i].offset, expected.keys[i].offset);
    }
}

void ExpectAmountEq(const asset::FluidAmount& actual, const asset::FluidAmount& expected)
{
    ASSERT_EQ(actual.keys.size(), expected.keys.size());
    for (std::size_t i = 0; i < actual.keys.size(); ++i) {
        EXPECT_FLOAT_EQ(actual.keys[i].time, expected.keys[i].time);
        EXPECT_FLOAT_EQ(actual.keys[i].scale, expected.keys[i].scale);
    }
}

void ExpectSourceEq(const asset::FluidSource& actual, const asset::FluidSource& expected)
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

void ExpectForceEq(const asset::FluidForce& actual, const asset::FluidForce& expected)
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

void ExpectColliderEq(const asset::FluidCollider& actual, const asset::FluidCollider& expected)
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

void ExpectRampEq(const asset::FluidColorRamp& actual, const asset::FluidColorRamp& expected)
{
    for (std::size_t i = 0; i < actual.stops.size(); ++i) {
        ExpectVector3Eq(actual.stops[i].color, expected.stops[i].color);
        EXPECT_FLOAT_EQ(actual.stops[i].position, expected.stops[i].position);
    }
}

/// 保存して読み直す。
asset::FluidRecipe RoundTrip(const testkit::TempDir& temp, const char* fileName, const asset::FluidRecipe& recipe)
{
    const std::string path = util::FileSystem::PathToUtf8(temp.File(fileName));
    EXPECT_TRUE(asset::SaveFluidRecipe(path, recipe));
    asset::FluidRecipe loaded;
    EXPECT_TRUE(asset::LoadFluidRecipe(path, loaded));
    return loaded;
}

/// TOML の文字列をファイルに書いて読む。
asset::FluidRecipe LoadText(const testkit::TempDir& temp, const char* fileName, const std::string& text)
{
    const std::filesystem::path file = temp.File(fileName);
    EXPECT_TRUE(util::FileSystem::WriteText(file, text));
    asset::FluidRecipe loaded;
    EXPECT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(file), loaded));
    return loaded;
}

float ToLinear(float c) { return std::pow(c, 2.2f); }

/// 無圧縮 32bit の TGA を書く (rgba は上の行から)。stb_image が読める一番単純な形で、PNG の符号化器を要らなくする。
bool WriteTgaRgba8(const std::filesystem::path& file, int width, int height, const std::vector<std::uint8_t>& rgba)
{
    std::vector<std::uint8_t> bytes(18, 0);
    bytes[2] = 2;   // 無圧縮 true color
    bytes[12] = static_cast<std::uint8_t>(width & 0xFF);
    bytes[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);
    bytes[14] = static_cast<std::uint8_t>(height & 0xFF);
    bytes[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    bytes[16] = 32;
    bytes[17] = 0x28;   // α 8bit + 原点は左上
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        bytes.push_back(rgba[i + 2]);   // TGA は BGRA
        bytes.push_back(rgba[i + 1]);
        bytes.push_back(rgba[i + 0]);
        bytes.push_back(rgba[i + 3]);
    }
    return util::FileSystem::WriteBinary(file, bytes.data(), bytes.size());
}

} // namespace

TEST(FluidRecipeBakeTest, BakeSectionRoundTripsThroughToml)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.bake = NonDefaultBake();
    const std::string path = util::FileSystem::PathToUtf8(temp.File("smoke.fluid"));
    ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
    asset::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(path, loaded));
    ExpectBakeEq(loaded.bake, recipe.bake);
}

TEST(FluidRecipeBakeTest, FileWithoutBakeSectionLoadsAs2D)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    const std::filesystem::path file = temp.File("old.fluid");
    ASSERT_TRUE(util::FileSystem::WriteText(file, "version = 1\nkind = \"gas\"\n\n[output]\ncolumns = 4\n"));
    asset::FluidRecipe loaded;
    ASSERT_TRUE(asset::LoadFluidRecipe(util::FileSystem::PathToUtf8(file), loaded));
    EXPECT_EQ(loaded.output.columns, 4);
    EXPECT_EQ(loaded.bake.mode, asset::FluidBakeMode::Flat2D);
    ExpectBakeEq(loaded.bake, asset::FluidBakeSettings{});
}

TEST(FluidRecipeBakeTest, AllPresetsBakeIn3DAndLiquidsStayOnCpu)
{
    // 3D の焼きがループと歪みマップを持ったので、ループものも陽炎も 3D。
    // 液体は GPU の粒子ソルバーが未検証のうちは CPU に置く。
    for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i) {
        const auto preset = static_cast<asset::FluidPreset>(i);
        SCOPED_TRACE(asset::FluidPresetName(preset));
        const asset::FluidRecipe recipe = asset::MakeFluidPreset(preset);
        EXPECT_EQ(recipe.bake.mode, asset::FluidBakeMode::Volume3D);
        if (recipe.kind == asset::FluidKind::Liquid) {
            EXPECT_EQ(recipe.bake.solver, asset::FluidBakeSolver::Cpu);
            EXPECT_EQ(recipe.bake.volumeResolution, 64);
            EXPECT_LE(recipe.bake.volumeResolution, 96);   // CPU の上限
            EXPECT_EQ(recipe.output.supersampling, 2);
        } else {
            EXPECT_EQ(recipe.bake.volumeResolution, 128);
            EXPECT_EQ(recipe.output.supersampling, 2);
            const bool lit = recipe.render.shading == asset::FluidShading::Smoke
                          || recipe.render.shading == asset::FluidShading::Fire;
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

    // 配列は要素が無いとキーが出ないので、発生源・力・動き・量のキーすべてに 1 つ以上入れる。
    // Enum は既定以外の値にする。
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    asset::FluidSource& moving = recipe.sources.emplace_back();
    moving.name = "Moving";
    moving.shape = asset::FluidSourceShape::Box;
    moving.motion.keys = { asset::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                           asset::FluidMotionKey{ 1.0f, { 0.2f, 0.0f, 0.0f } } };
    moving.amount.keys = { asset::FluidAmountKey{ 0.0f, 1.0f }, asset::FluidAmountKey{ 0.4f, 0.25f } };
    asset::FluidForce& gust = recipe.forces.emplace_back();
    gust.name = "Gust";
    gust.type = asset::FluidForceType::Noise;
    gust.motion.keys = { asset::FluidMotionKey{ 0.5f, { 0.0f, 0.1f, 0.0f } } };
    gust.amount.keys = { asset::FluidAmountKey{ 0.2f, 0.5f } };
    asset::FluidSource& stamp = recipe.sources.emplace_back();
    stamp.name = "Stamp";
    stamp.shape = asset::FluidSourceShape::Texture;
    stamp.texture = "Textures/FX/Logo.png";
    asset::FluidCollider& wall = recipe.colliders.emplace_back();
    wall.name = "Wall";
    wall.shape = asset::FluidColliderShape::Plane;
    wall.motion.keys = { asset::FluidMotionKey{ 0.25f, { 0.1f, 0.0f, 0.0f } } };
    recipe.kind = asset::FluidKind::Liquid;
    recipe.render.shading = asset::FluidShading::Glow;
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
    // 部品の name / texture は自由な文字列で Enum ではない。選択肢の比較からは外す。
    std::erase_if(tomlStrings, [](const KeyValue& entry) {
        return entry.first.ends_with("[].name") || entry.first.ends_with("[].texture");
    });

    KeyRecorder recorder;
    asset::ReflectFluidRecipe(recipe, recorder);

    EXPECT_TRUE(Difference(recorder.keys, tomlKeys).empty())
        << "TOML に無い Reflect 名: " << Join(Difference(recorder.keys, tomlKeys));
    EXPECT_TRUE(Difference(tomlKeys, recorder.keys).empty())
        << "Reflect が触らない TOML キー: " << Join(Difference(tomlKeys, recorder.keys));
    // Enum の選択肢の文字列 = TOML に書く名前 (JSON は添字、TOML は名前で同じ値を指す)。
    EXPECT_EQ(recorder.enumValues, tomlStrings);
    // 部品の配列は期待した名前で出ている (上の一致だけだと、両方が同じ名前で間違っていても通る)。
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

    asset::FluidRecipe recipe;
    asset::FluidSource ring;
    ring.enabled = false;
    ring.name = "Shock";
    ring.shape = asset::FluidSourceShape::Ring;
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
    ring.motion.keys = { asset::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                         asset::FluidMotionKey{ 0.5f, { 0.3f, 0.1f, 0.0f } },
                         asset::FluidMotionKey{ 1.5f, { -0.2f, 0.4f, 0.1f } } };
    recipe.sources.push_back(ring);
    asset::FluidSource cone;
    cone.shape = asset::FluidSourceShape::Cone;
    recipe.sources.push_back(cone);

    asset::FluidForce vortex;
    vortex.name = "Spin";
    vortex.type = asset::FluidForceType::Vortex;
    vortex.center = { -0.3f, 0.2f, 0.0f };
    vortex.direction = { 0.0f, 1.0f, 0.0f };
    vortex.strength = -3.5f;
    vortex.radius = 0.6f;
    vortex.falloffPower = 1.5f;
    vortex.noiseFrequency = 5.0f;
    vortex.noiseSpeed = 0.25f;
    vortex.startTime = 0.1f;
    vortex.duration = 2.0f;
    vortex.motion.keys = { asset::FluidMotionKey{ 0.25f, { 0.0f, 0.5f, 0.0f } } };
    recipe.forces.push_back(vortex);
    asset::FluidForce drag;
    drag.type = asset::FluidForceType::Drag;
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

    const asset::FluidRecipe loaded = RoundTrip(temp, "parts.fluid", recipe);
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

    asset::FluidRecipe recipe;
    asset::FluidSource stamp;
    stamp.name = "Sigil";
    stamp.shape = asset::FluidSourceShape::Texture;
    stamp.texture = "guid:0123456789abcdef0123456789abcdef";
    stamp.size = { 0.5f, 0.25f, 0.04f };
    stamp.direction = { 0.0f, 0.0f, 1.0f };
    recipe.sources.push_back(stamp);

    asset::FluidCollider ball;
    ball.enabled = false;
    ball.name = "Ball";
    ball.center = { 0.1f, 0.2f, -0.3f };
    ball.size = { 0.3f, 0.0f, 0.0f };
    ball.friction = 0.9f;
    ball.startTime = 0.25f;
    ball.duration = 1.5f;
    ball.motion.inheritVelocity = false;
    ball.motion.keys = { asset::FluidMotionKey{ 0.0f, { 0.0f, 0.0f, 0.0f } },
                         asset::FluidMotionKey{ 1.0f, { 0.5f, 0.0f, 0.0f } } };
    recipe.colliders.push_back(ball);
    asset::FluidCollider box;
    box.shape = asset::FluidColliderShape::Box;
    box.size = { 0.2f, 0.1f, 0.3f };
    recipe.colliders.push_back(box);
    asset::FluidCollider ramp;
    ramp.shape = asset::FluidColliderShape::Plane;
    ramp.center = { 0.0f, -0.5f, 0.0f };
    ramp.direction = { -0.5f, 1.0f, 0.0f };
    recipe.colliders.push_back(ramp);

    const asset::FluidRecipe loaded = RoundTrip(temp, "colliders.fluid", recipe);
    ASSERT_EQ(loaded.sources.size(), 1u);
    ExpectSourceEq(loaded.sources[0], stamp);
    ASSERT_EQ(loaded.colliders.size(), recipe.colliders.size());
    for (std::size_t i = 0; i < loaded.colliders.size(); ++i) ExpectColliderEq(loaded.colliders[i], recipe.colliders[i]);

    // 形は名前で書く (JSON の添字ではない)。
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

    const asset::FluidRecipe loaded = LoadText(temp, "collider.fluid",
                                               "version = 2\n"
                                               "kind = \"liquid\"\n"
                                               "\n[[collider]]\n"
                                               // 知らない名前 (将来の形) は既定の sphere に落ちる。
                                               "shape = \"torus\"\n"
                                               "friction = 1.25\n"
                                               "\n[[collider.motion.key]]\ntime = 0.5\noffset = [0.0, 0.2, 0.0]\n"
                                               "\n[[collider.motion.key]]\ntime = 0.1\noffset = [0.0, 0.1, 0.0]\n");
    ASSERT_EQ(loaded.colliders.size(), 1u);
    const asset::FluidCollider& collider = loaded.colliders[0];
    const asset::FluidCollider defaults;
    EXPECT_EQ(collider.shape, asset::FluidColliderShape::Sphere);
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

    // 使われていなかった側 (気体のレシピの liquid_emitter) は移さない。
    const asset::FluidRecipe loaded = LoadText(temp, "v1gas.fluid",
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
    EXPECT_EQ(loaded.kind, asset::FluidKind::Gas);
    ASSERT_EQ(loaded.sources.size(), 2u);
    EXPECT_TRUE(loaded.forces.empty());

    asset::FluidSource expected;
    expected.shape = asset::FluidSourceShape::Box;
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

    asset::FluidSource second;
    second.center = { 0.0f, 0.0f, 0.0f };
    ExpectSourceEq(loaded.sources[1], second);
}

TEST(FluidRecipeBakeTest, Version1LiquidEmitterKeepsItsOldDefaults)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    // version の無いファイルは version 1。欠けたキーは当時の液体の既定で埋まる (今の FluidSource の既定ではない)。
    const asset::FluidRecipe loaded = LoadText(temp, "v1liquid.fluid",
                                               "kind = \"liquid\"\n"
                                               "\n[[liquid_emitter]]\n"
                                               "count = 42\n"
                                               "\n[[gas_source]]\n"
                                               "density = 9.0\n");
    EXPECT_EQ(loaded.kind, asset::FluidKind::Liquid);
    ASSERT_EQ(loaded.sources.size(), 1u);
    const asset::FluidSource& source = loaded.sources[0];
    EXPECT_EQ(source.count, 42);
    EXPECT_EQ(source.shape, asset::FluidSourceShape::Sphere);
    ExpectVector3Eq(source.center, { 0.0f, -0.6f, 0.0f });
    ExpectVector3Eq(source.size, { 0.08f, 0.08f, 0.08f });
    ExpectVector3Eq(source.velocity, { 0.0f, 2.5f, 0.0f });
    EXPECT_FLOAT_EQ(source.spread, 0.5f);
    EXPECT_FLOAT_EQ(source.startTime, 0.0f);
    EXPECT_FLOAT_EQ(source.duration, 0.15f);
    EXPECT_TRUE(source.enabled);
    EXPECT_TRUE(source.motion.keys.empty());

    // 書かれていたキーはそのまま移る。
    const asset::FluidRecipe full = LoadText(temp, "v1liquid_full.fluid",
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
    // 先頭の発生源にだけ上限を超えるキーを持たせる。
    text += "\n[[source]]\nname = \"keys\"\n";
    for (int i = 0; i < asset::kMaxFluidMotionKeys + 4; ++i)
        text += "\n[[source.motion.key]]\ntime = " + std::to_string(i) + ".0\n";
    for (int i = 1; i < asset::kMaxFluidSources + 4; ++i)
        text += "\n[[source]]\ndensity = " + std::to_string(i) + ".0\n";
    for (int i = 0; i < asset::kMaxFluidForces + 3; ++i)
        text += "\n[[force]]\nstrength = " + std::to_string(i) + ".0\n";
    for (int i = 0; i < asset::kMaxFluidColliders + 3; ++i)
        text += "\n[[collider]]\nfriction = " + std::to_string(i) + ".0\n";

    const asset::FluidRecipe loaded = LoadText(temp, "many.fluid", text);
    ASSERT_EQ(loaded.sources.size(), static_cast<std::size_t>(asset::kMaxFluidSources));
    ASSERT_EQ(loaded.forces.size(), static_cast<std::size_t>(asset::kMaxFluidForces));
    ASSERT_EQ(loaded.colliders.size(), static_cast<std::size_t>(asset::kMaxFluidColliders));
    EXPECT_FLOAT_EQ(loaded.colliders.back().friction, static_cast<float>(asset::kMaxFluidColliders - 1));
    // ファイルの先頭から上限までが残る。
    EXPECT_EQ(loaded.sources[0].name, "keys");
    EXPECT_FLOAT_EQ(loaded.sources.back().density, static_cast<float>(asset::kMaxFluidSources - 1));
    EXPECT_FLOAT_EQ(loaded.forces.back().strength, static_cast<float>(asset::kMaxFluidForces - 1));
    const std::vector<asset::FluidMotionKey>& keys = loaded.sources[0].motion.keys;
    ASSERT_EQ(keys.size(), static_cast<std::size_t>(asset::kMaxFluidMotionKeys));
    EXPECT_FLOAT_EQ(keys.back().time, static_cast<float>(asset::kMaxFluidMotionKeys - 1));
}

TEST(FluidRecipeBakeTest, MotionKeysAreSortedByTimeOnLoad)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    const asset::FluidRecipe loaded = LoadText(temp, "unsorted.fluid",
                                               "version = 2\n"
                                               "kind = \"gas\"\n"
                                               "\n[[force]]\n"
                                               "type = \"attract\"\n"
                                               "\n[[force.motion.key]]\ntime = 0.5\noffset = [0.5, 0.0, 0.0]\n"
                                               "\n[[force.motion.key]]\ntime = 0.1\noffset = [0.1, 0.0, 0.0]\n"
                                               "\n[[force.motion.key]]\ntime = 0.3\noffset = [0.3, 0.0, 0.0]\n");
    ASSERT_EQ(loaded.forces.size(), 1u);
    // [[collider]] より前の version 2 のファイルは障害物なしで開ける。
    EXPECT_TRUE(loaded.colliders.empty());
    EXPECT_EQ(loaded.forces[0].type, asset::FluidForceType::Attract);
    const std::vector<asset::FluidMotionKey>& keys = loaded.forces[0].motion.keys;
    ASSERT_EQ(keys.size(), 3u);
    const float times[] = { 0.1f, 0.3f, 0.5f };
    for (std::size_t i = 0; i < keys.size(); ++i) {
        EXPECT_FLOAT_EQ(keys[i].time, times[i]);
        EXPECT_FLOAT_EQ(keys[i].offset.x, times[i]);   // offset はキーと一緒に並び替わる
    }
}

TEST(FluidRecipeBakeTest, EveryPresetRoundTripsAndHasAnEnabledSource)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i) {
        const auto preset = static_cast<asset::FluidPreset>(i);
        SCOPED_TRACE(asset::FluidPresetName(preset));
        const asset::FluidRecipe recipe = asset::MakeFluidPreset(preset);
        EXPECT_TRUE(std::any_of(recipe.sources.begin(), recipe.sources.end(),
                                [](const asset::FluidSource& source) { return source.enabled; }));
        EXPECT_LE(recipe.sources.size(), static_cast<std::size_t>(asset::kMaxFluidSources));
        EXPECT_LE(recipe.forces.size(), static_cast<std::size_t>(asset::kMaxFluidForces));
        for (const asset::FluidSource& source : recipe.sources) EXPECT_FALSE(source.name.empty());
        for (const asset::FluidForce& force : recipe.forces) EXPECT_FALSE(force.name.empty());
        EXPECT_LE(recipe.colliders.size(), static_cast<std::size_t>(asset::kMaxFluidColliders));
        for (const asset::FluidCollider& collider : recipe.colliders) EXPECT_FALSE(collider.name.empty());
        // 同梱の画像が無いので、Texture の発生源を使うプリセットは作らない (焼くと板の形がそのまま出る)。
        for (const asset::FluidSource& source : recipe.sources)
            EXPECT_NE(source.shape, asset::FluidSourceShape::Texture);

        const asset::FluidRecipe loaded = RoundTrip(temp, "preset.fluid", recipe);
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
    // 液体のプリセットは旧 emitter の値を FluidSource へ移している。先頭がその emitter で、
    // «球・撃ち出す速度・ばらつき・数・出し切る時間» を今も FluidSource 側で持っていることを見る。
    // WHY 数値そのものを縛らないか: ここは移し替えの網であって、見た目の決定ではない。値を固定すると
    //     プリセットの絵を詰め直せなくなる (枠からはみ出す飛沫を抑える調整が «テスト違反» になる)。
    const asset::FluidRecipe splash = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    ASSERT_GE(splash.sources.size(), 1u);
    EXPECT_EQ(splash.sources[0].shape, asset::FluidSourceShape::Sphere);
    EXPECT_GT(splash.sources[0].size.x, 0.0f);
    EXPECT_GT(splash.sources[0].velocity.y, 0.0f);
    EXPECT_GT(splash.sources[0].spread, 0.0f);
    EXPECT_GT(splash.sources[0].count, 0);
    EXPECT_GT(splash.sources[0].duration, 0.0f);

    const asset::FluidRecipe steam = asset::MakeFluidPreset(asset::FluidPreset::Steam);
    ASSERT_FALSE(steam.sources.empty());
    EXPECT_EQ(steam.sources[0].shape, asset::FluidSourceShape::Cone);

    const asset::FluidRecipe wisp = asset::MakeFluidPreset(asset::FluidPreset::MagicWisp);
    ASSERT_GE(wisp.forces.size(), 1u);
    EXPECT_EQ(wisp.forces[0].type, asset::FluidForceType::Vortex);

    const asset::FluidRecipe ink = asset::MakeFluidPreset(asset::FluidPreset::Ink);
    ASSERT_FALSE(ink.sources.empty());
    EXPECT_GE(ink.sources[0].motion.keys.size(), 2u);

    // 発生源ごとの色の見本は爆発: 芯と外側の土煙で色の鍵が違い、albedo_ramp を使う。
    const asset::FluidRecipe boom = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    EXPECT_TRUE(boom.render.useAlbedoRamp);
    ASSERT_GE(boom.sources.size(), 2u);
    EXPECT_NE(boom.sources[0].colorKey, boom.sources[1].colorKey);
    for (std::size_t i = 1; i < boom.render.albedoRamp.stops.size(); ++i)
        EXPECT_LE(boom.render.albedoRamp.stops[i - 1].position, boom.render.albedoRamp.stops[i].position);

    // 障害物の見本は噴流の岩 1 つだけ (他のプリセットの絵は変えない)。岩は床に接地している。
    int withColliders = 0;
    for (int i = 0; i < static_cast<int>(asset::FluidPreset::Count); ++i)
        if (!asset::MakeFluidPreset(static_cast<asset::FluidPreset>(i)).colliders.empty()) ++withColliders;
    EXPECT_EQ(withColliders, 1);
    const asset::FluidRecipe jet = asset::MakeFluidPreset(asset::FluidPreset::WaterJet);
    ASSERT_EQ(jet.colliders.size(), 1u);
    EXPECT_EQ(jet.colliders[0].shape, asset::FluidColliderShape::Box);
    EXPECT_NEAR(jet.colliders[0].center.y - jet.colliders[0].size.y, jet.liquid.floorHeight, 1.0e-5f);
}

TEST(FluidRecipeBakeTest, VolumeSettingsComeFromOutputAndRender)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
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
    EXPECT_EQ(settings.supersampling, 3);   // Volume Baker の上限
    EXPECT_FLOAT_EQ(settings.detailStrength, 0.4f);
    EXPECT_FLOAT_EQ(settings.detailScale, 12.0f);
    EXPECT_FLOAT_EQ(settings.detailPeriod, 0.6f);
    EXPECT_EQ(settings.sourceKind, asset::VolumeSourceKind::Fluid);
    EXPECT_EQ(settings.fluidRecipePath, path);
    EXPECT_EQ(settings.fluidSolver, asset::VolumeFluidSolver::Cpu);
    EXPECT_FLOAT_EQ(settings.fluidDensityScale, 2.5f);
    EXPECT_EQ(settings.volumeResolution, 96);
    EXPECT_FLOAT_EQ(settings.exposure, 1.3f);
    EXPECT_FLOAT_EQ(settings.halfExtent, 1.4f);
    EXPECT_TRUE(settings.blackbodyEmission);
    // path は gtest の表示でコンテナ扱いされるので、比較だけして文字列で報告する。
    EXPECT_TRUE(util::FileSystem::PathFromUtf8(settings.outputDirectory).lexically_normal()
                == temp.Path().lexically_normal())
        << settings.outputDirectory;
    EXPECT_EQ(settings.baseName, "MySmoke");

    // 煙は温度を持つが光らせない。炎は炎の Ramp。
    const math::Vector3 hottestSmoke = asset::EvaluateVolumeRamp(settings.emissionRamp, 1.0f);
    EXPECT_FLOAT_EQ(hottestSmoke.x + hottestSmoke.y + hottestSmoke.z, 0.0f);
    recipe.render.shading = asset::FluidShading::Fire;
    const math::Vector3 hottestFire =
        asset::EvaluateVolumeRamp(asset::MakeVolumeBakeSettings(recipe, path).emissionRamp, 1.0f);
    ExpectVector3Eq(hottestFire, asset::EvaluateVolumeRamp(asset::DefaultFireRamp(), 1.0f));

    // コマ数は Volume Baker の範囲 [2, 256] に丸めてから間隔を割る。
    recipe.output.columns = 32;
    recipe.output.rows = 32;
    const asset::VolumeFlipbookBakeSettings many = asset::MakeVolumeBakeSettings(recipe, path);
    EXPECT_EQ(many.source.frameCount, 256);
    EXPECT_FLOAT_EQ(many.source.frameDt, 3.0f / 256.0f);
    recipe.output.columns = 1;
    recipe.output.rows = 1;
    EXPECT_EQ(asset::MakeVolumeBakeSettings(recipe, path).source.frameCount, 2);
}

TEST(FluidRecipeBakeTest, UserEmissionRampReachesTheVolumeBaker)
{
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.render.useEmissionRamp = true;
    recipe.render.emissionRamp.stops[2] = { { 0.1f, 0.8f, 2.0f }, 0.5f };
    recipe.render.emissionRamp.stops[3] = { { 7.0f, 1.0f, 0.5f }, 1.0f };

    // 煙でも Ramp を指定すればその色で光る (shading によらない)。リニアのまま写す。
    const asset::VolumeFlipbookBakeSettings settings = asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Tinted.fluid");
    for (std::size_t i = 0; i < settings.emissionRamp.stops.size(); ++i) {
        ExpectVector3Eq(settings.emissionRamp.stops[i].color, recipe.render.emissionRamp.stops[i].color);
        EXPECT_FLOAT_EQ(settings.emissionRamp.stops[i].position, recipe.render.emissionRamp.stops[i].position);
    }
    recipe.render.shading = asset::FluidShading::Fire;
    ExpectVector3Eq(asset::EvaluateVolumeRamp(asset::MakeVolumeBakeSettings(recipe, "C:/Fx/Tinted.fluid").emissionRamp,
                                              1.0f),
                    { 7.0f, 1.0f, 0.5f });
}

TEST(FluidRecipeBakeTest, LiquidAlbedoIsTheLiquidColor)
{
    const asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
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
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
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
    asset::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    recipe.bake = NonDefaultBake();
    const std::string path = "C:/Fx/Boom.fluid";

    asset::FluidRecipe stored = recipe;
    asset::StoreVolumeBakeSettings(asset::MakeVolumeBakeSettings(recipe, path), stored);
    ExpectBakeEq(stored.bake, recipe.bake);

    // パネルで触った値は [bake] へ戻り、[output] (コマ数・タイル) は触らない。
    asset::VolumeFlipbookBakeSettings edited = asset::MakeVolumeBakeSettings(recipe, path);
    edited.exposure = 2.0f;
    edited.fluidSolver = asset::VolumeFluidSolver::Gpu;
    edited.tileSize = 512;
    edited.source.frameCount = 12;
    asset::StoreVolumeBakeSettings(edited, stored);
    EXPECT_FLOAT_EQ(stored.bake.exposure, 2.0f);
    EXPECT_EQ(stored.bake.solver, asset::FluidBakeSolver::Gpu);
    EXPECT_EQ(stored.bake.mode, recipe.bake.mode);
    EXPECT_EQ(stored.output.frameSize, recipe.output.frameSize);
    EXPECT_EQ(stored.output.columns, recipe.output.columns);
    EXPECT_EQ(stored.output.rows, recipe.output.rows);
}

TEST(FluidRecipeBakeTest, AlbedoRampIsSortedOnLoadAndOldFilesKeepItOff)
{
    testkit::TempDir temp{ "fluidbake" };
    ASSERT_TRUE(temp.IsValid());

    // albedo_ramp / color_key の無い古いファイルは 1 色のまま (鍵 0)。
    const asset::FluidRecipe old = LoadText(temp, "old_albedo.fluid",
                                            "version = 2\nkind = \"gas\"\n\n[[source]]\nname = \"a\"\n");
    EXPECT_FALSE(old.render.useAlbedoRamp);
    ExpectRampEq(old.render.albedoRamp, asset::FluidRenderSettings{}.albedoRamp);
    ASSERT_EQ(old.sources.size(), 1u);
    EXPECT_FLOAT_EQ(old.sources[0].colorKey, 0.0f);

    const asset::FluidRecipe loaded = LoadText(temp, "unsorted_albedo.fluid",
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
    ExpectVector3Eq(stops[0].color, { 0.0f, 1.0f, 0.0f });   // 色は点と一緒に並び替わる
    ExpectVector3Eq(stops[3].color, { 1.0f, 0.0f, 0.0f });
    ASSERT_EQ(loaded.sources.size(), 1u);
    EXPECT_FLOAT_EQ(loaded.sources[0].colorKey, 0.4f);
}

TEST(FluidRecipeBakeTest, VolumeSettingsCarryLoopDistortionAndAlbedoRamp)
{
    const auto expectRamp = [](const asset::VolumeColorRamp& actual, const asset::FluidColorRamp& expected) {
        for (std::size_t i = 0; i < actual.stops.size(); ++i) {
            ExpectVector3Eq(actual.stops[i].color, expected.stops[i].color);
            EXPECT_FLOAT_EQ(actual.stops[i].position, expected.stops[i].position);
        }
    };

    // 陽炎: ループし、色の代わりに歪みを焼く。倍率は 2D の符号化 (速さ 1 で変位 0.42) と同じ。
    const asset::FluidRecipe haze = asset::MakeFluidPreset(asset::FluidPreset::HeatHaze);
    const asset::VolumeFlipbookBakeSettings hazeSettings = asset::MakeVolumeBakeSettings(haze, "C:/Fx/Haze.fluid");
    EXPECT_TRUE(hazeSettings.fluidLoop);
    EXPECT_TRUE(hazeSettings.distortion);
    EXPECT_FLOAT_EQ(hazeSettings.distortionScale, 0.42f);

    // 一度きりの煙はループも歪みもしない。
    const asset::VolumeFlipbookBakeSettings smoke =
        asset::MakeVolumeBakeSettings(asset::MakeFluidPreset(asset::FluidPreset::Smoke), "C:/Fx/Smoke.fluid");
    EXPECT_FALSE(smoke.fluidLoop);
    EXPECT_FALSE(smoke.distortion);
    EXPECT_TRUE(asset::MakeVolumeBakeSettings(asset::MakeFluidPreset(asset::FluidPreset::Fire), "C:/Fx/Fire.fluid")
                    .fluidLoop);

    // 気体: albedo_ramp はリニアのまま写す。切れば smoke_color の 1 色。
    asset::FluidRecipe boom = asset::MakeFluidPreset(asset::FluidPreset::Explosion);
    ASSERT_TRUE(boom.render.useAlbedoRamp);
    expectRamp(asset::MakeVolumeBakeSettings(boom, "C:/Fx/Boom.fluid").albedoRamp, boom.render.albedoRamp);
    boom.render.useAlbedoRamp = false;
    const asset::VolumeFlipbookBakeSettings plain = asset::MakeVolumeBakeSettings(boom, "C:/Fx/Boom.fluid");
    for (const asset::VolumeRampStop& stop : plain.albedoRamp.stops) {
        EXPECT_NEAR(stop.color.x, ToLinear(boom.render.smokeColor.x), 1.0e-5f);
        EXPECT_NEAR(stop.color.y, ToLinear(boom.render.smokeColor.y), 1.0e-5f);
        EXPECT_NEAR(stop.color.z, ToLinear(boom.render.smokeColor.z), 1.0e-5f);
    }

    // 液体: albedo_ramp を使うなら liquid_color の 1 色の代わりに Ramp。
    asset::FluidRecipe splash = asset::MakeFluidPreset(asset::FluidPreset::WaterSplash);
    splash.render.useAlbedoRamp = true;
    splash.render.albedoRamp.stops[0] = { { 0.9f, 0.1f, 0.1f }, 0.0f };
    splash.render.albedoRamp.stops[3] = { { 0.1f, 0.2f, 0.9f }, 1.0f };
    expectRamp(asset::MakeVolumeBakeSettings(splash, "C:/Fx/Splash.fluid").albedoRamp, splash.render.albedoRamp);
}

TEST(FluidSourceMaskTest, ValueIsLuminanceTimesAlphaWithTopRowFirst)
{
    testkit::TempDir temp{ "fluidmask" };
    ASSERT_TRUE(temp.IsValid());

    // 左半分は白 (下 1/4 だけ黒)、右半分は透明な白。縦横比は 2:1 (マスクは正方形へ引き伸ばす)。
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

    asset::FluidSourceMask mask;
    std::string error;
    ASSERT_TRUE(asset::LoadFluidSourceMask(util::FileSystem::PathToUtf8(file), mask, &error)) << error;
    ASSERT_TRUE(mask.IsValid());
    EXPECT_NEAR(asset::SampleFluidSourceMask(mask, 0.1f, 0.1f), 1.0f, 1.0e-3f);    // 白・不透明
    EXPECT_NEAR(asset::SampleFluidSourceMask(mask, 0.9f, 0.5f), 0.0f, 1.0e-3f);    // 透明
    EXPECT_NEAR(asset::SampleFluidSourceMask(mask, 0.1f, 0.95f), 0.0f, 1.0e-3f);   // 下の黒 (v は上が 0)
    // 範囲外は縁の値。
    EXPECT_FLOAT_EQ(asset::SampleFluidSourceMask(mask, -3.0f, 0.1f), asset::SampleFluidSourceMask(mask, 0.0f, 0.1f));
    EXPECT_FLOAT_EQ(asset::SampleFluidSourceMask(mask, 0.1f, 5.0f), asset::SampleFluidSourceMask(mask, 0.1f, 1.0f));
    EXPECT_NEAR(asset::SampleFluidSourceMask(mask, 0.0f, 0.0f), 1.0f, 1.0e-3f);
    for (const float value : mask.values) {
        ASSERT_GE(value, 0.0f);
        ASSERT_LE(value, 1.0f);
    }
}

TEST(FluidSourceMaskTest, ShrinkingKeepsThinLines)
{
    testkit::TempDir temp{ "fluidmask" };
    ASSERT_TRUE(temp.IsValid());

    // 4 列に 1 本の白線を 1/4 に縮める。点で拾う双線形だと線の間だけを拾って 0 になる。
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

    asset::FluidSourceMask mask;
    ASSERT_TRUE(asset::LoadFluidSourceMask(util::FileSystem::PathToUtf8(file), mask));
    for (const float u : { 0.1f, 0.37f, 0.5f, 0.83f })
        EXPECT_NEAR(asset::SampleFluidSourceMask(mask, u, 0.5f), 0.25f, 1.0e-3f) << u;
}

TEST(FluidSourceMaskTest, MissingOrBrokenFileLeavesTheMaskInvalid)
{
    testkit::TempDir temp{ "fluidmask" };
    ASSERT_TRUE(temp.IsValid());

    asset::FluidSourceMask mask;
    std::string error;
    EXPECT_FALSE(asset::LoadFluidSourceMask(util::FileSystem::PathToUtf8(temp.File("missing.png")), mask, &error));
    EXPECT_FALSE(mask.IsValid());
    EXPECT_FALSE(error.empty());
    // 読めていないマスクは 1 (板の形のまま湧く)。
    EXPECT_FLOAT_EQ(asset::SampleFluidSourceMask(mask, 0.3f, 0.7f), 1.0f);

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

    // 4 象限のシート。左上だけ白 (それ以外は黒)。コマを切り抜けていれば «全部 1» と «全部 0» に割れる。
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
        std::filesystem::path(file).concat(".meta"),
        std::string(R"([texture]
type = "Sprite"
sprite_mode = "Multiple"
sprites = [
  { id = "id-lit", name = "Lit", x = 0, y = 0, width = 32, height = 32 },
  { id = "id-dark", name = "Dark", x = 32, y = 32, width = 32, height = 32 },
]
)")));

    const std::string image = util::FileSystem::PathToUtf8(file);
    asset::FluidSourceMask mask;
    std::string error;

    // ID でも名前でも同じコマを指す。
    for (const char* token : { "id-lit", "Lit" }) {
        ASSERT_TRUE(asset::LoadFluidSourceMask(asset::MakeSpriteReference(image, token), mask, &error)) << error;
        ASSERT_TRUE(mask.IsValid());
        for (const float u : { 0.05f, 0.5f, 0.95f })
            EXPECT_NEAR(asset::SampleFluidSourceMask(mask, u, u), 1.0f, 1.0e-3f) << token << " " << u;
    }

    ASSERT_TRUE(asset::LoadFluidSourceMask(asset::MakeSpriteReference(image, "Dark"), mask, &error)) << error;
    for (const float u : { 0.05f, 0.5f, 0.95f })
        EXPECT_NEAR(asset::SampleFluidSourceMask(mask, u, u), 0.0f, 1.0e-3f) << u;

    // 参照のまま (切り抜かずに) 読むとシート全体なので、左上の 1/4 だけが白い。
    ASSERT_TRUE(asset::LoadFluidSourceMask(image, mask, &error)) << error;
    EXPECT_NEAR(asset::SampleFluidSourceMask(mask, 0.25f, 0.25f), 1.0f, 1.0e-3f);
    EXPECT_NEAR(asset::SampleFluidSourceMask(mask, 0.75f, 0.75f), 0.0f, 1.0e-3f);

    // 切れた参照はアトラス全面へ落とさず «読めない» にする (呼び手が赤く言える)。
    EXPECT_FALSE(asset::LoadFluidSourceMask(asset::MakeSpriteReference(image, "Gone"), mask, &error));
    EXPECT_FALSE(mask.IsValid());
    EXPECT_FALSE(error.empty());
}

} // namespace fbzz::tests
