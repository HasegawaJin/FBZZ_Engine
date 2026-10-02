/// @file    MaterialReflectionTests.cpp
/// @brief   Material の数値精度・HLSL 配置と DataAsset の型付き保存を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace fbzz::tests {
namespace {

template<typename T>
T Read(const std::vector<uint8_t>& bytes, size_t offset)
{
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

renderer::ShaderVarDesc Scalar(std::string name, renderer::ShaderVarType type, uint32_t offset)
{
    renderer::ShaderVarDesc variable;
    variable.name = std::move(name);
    variable.varType = type;
    variable.offset = offset;
    variable.size = 4;
    return variable;
}

class ReflectionData final : public asset::DataAsset {
    FBZZ_DATA_ASSET(ReflectionData)
    FBZZ_FIELD(input::KeyCode, key, input::KeyCode::SPACE, "Key")
    FBZZ_ASSET_LIST_FIELD(scene::TextureRef, textures, "Textures")
    FBZZ_SERIALIZE_REFERENCE(config, "Config")
};
FBZZ_REFLECT(ReflectionData)

class NestedReflectionData final : public scene::IScriptSerializable {
    FBZZ_SERIALIZABLE(NestedReflectionData)
    FBZZ_FIELD(int, count, 0, "Count")
    FBZZ_ASSET_LIST_FIELD(scene::TextureRef, textures, "Textures")
};
FBZZ_REFLECT(NestedReflectionData)

}

class MaterialReflectionTest : public testkit::EngineFixture {};

TEST_F(MaterialReflectionTest, IntegerAndBoolOverridesReachGpuBytesWithoutFloatRounding)
{
    renderer::ShaderDescriptor descriptor;
    descriptor.cbufferSize = 16;
    descriptor.vars = {
        Scalar("signedValue", renderer::ShaderVarType::Int, 0),
        Scalar("unsignedValue", renderer::ShaderVarType::UInt, 4),
        Scalar("enabled", renderer::ShaderVarType::Bool, 8)
    };
    std::vector<uint8_t> bytes(16, 0xCD);
    const std::unordered_map<std::string, std::vector<int64_t>> overrides{
        {"signedValue", {INT32_MIN}}, {"unsignedValue", {UINT32_MAX}}, {"enabled", {1}}
    };
    asset::ApplyMaterialIntegerOverrides(overrides, descriptor, bytes);
    EXPECT_EQ(Read<int32_t>(bytes, 0), INT32_MIN);
    EXPECT_EQ(Read<uint32_t>(bytes, 4), UINT32_MAX);
    EXPECT_EQ(Read<uint32_t>(bytes, 8), 1u);
    EXPECT_EQ(Read<uint32_t>(bytes, 12), 0xCDCDCDCDu);
}

TEST_F(MaterialReflectionTest, ScalarArrayUsesSixteenByteStrideAndPreservesPadding)
{
    auto variable = Scalar("weights", renderer::ShaderVarType::Float, 4);
    variable.elements = 3;
    variable.arrayStride = 16;
    variable.size = 36;
    std::vector<uint8_t> bytes(48, 0xCD);
    const std::array<double, 3> values{2, 3, 5};
    ASSERT_TRUE(asset::WriteMaterialValues(variable, values, bytes));
    EXPECT_FLOAT_EQ(Read<float>(bytes, 4), 2);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 20), 3);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 36), 5);
    EXPECT_EQ(Read<uint32_t>(bytes, 8), 0xCDCDCDCDu);
    EXPECT_EQ(Read<uint32_t>(bytes, 40), 0xCDCDCDCDu);
}

TEST_F(MaterialReflectionTest, MatrixArraysRespectBothStorageOrders)
{
    renderer::ShaderVarDesc variable;
    variable.name = "matrices";
    variable.varClass = renderer::ShaderVarClass::Matrix;
    variable.rows = 2;
    variable.columns = 3;
    variable.elements = 2;
    const std::array<double, 12> values{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<uint8_t> bytes(96, 0xCD);
    variable.arrayStride = 48;
    variable.size = 88;
    ASSERT_TRUE(asset::WriteMaterialValues(variable, values, bytes));
    EXPECT_FLOAT_EQ(Read<float>(bytes, 0), 1);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 4), 4);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 16), 2);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 36), 6);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 48), 7);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 84), 12);
    EXPECT_EQ(Read<uint32_t>(bytes, 8), 0xCDCDCDCDu);
    std::fill(bytes.begin(), bytes.end(), 0xCD);
    variable.rowMajor = true;
    variable.arrayStride = 32;
    variable.size = 60;
    ASSERT_TRUE(asset::WriteMaterialValues(variable, values, bytes));
    EXPECT_FLOAT_EQ(Read<float>(bytes, 8), 3);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 16), 4);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 32), 7);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 56), 12);
    EXPECT_EQ(Read<uint32_t>(bytes, 12), 0xCDCDCDCDu);
}

TEST_F(MaterialReflectionTest, InvalidValuesAndTruncatedBuffersAreAtomic)
{
    auto variable = Scalar("value", renderer::ShaderVarType::UInt, 4);
    std::vector<uint8_t> bytes(8, 0xCD);
    const auto before = bytes;
    for (const double value : {-1.0, 1.5, 4294967296.0, std::numeric_limits<double>::infinity()}) {
        EXPECT_FALSE(asset::WriteMaterialValues(variable, std::span(&value, 1), bytes));
        EXPECT_EQ(bytes, before);
    }
    const std::array<double, 1> valid{42};
    EXPECT_FALSE(asset::WriteMaterialValues(variable, valid, std::span(bytes.data(), 7)));
    EXPECT_EQ(bytes, before);
    variable.varType = renderer::ShaderVarType::Bool;
    EXPECT_FALSE(asset::WriteMaterialValues(variable, valid, bytes));
    variable.unsupportedReason = "64-bit storage is unsupported";
    EXPECT_FALSE(asset::WriteMaterialValues(variable, valid, bytes));
}

TEST_F(MaterialReflectionTest, MaterialFilePreservesUnsignedLimitsAndLegacyFloatParams)
{
    testkit::TempDir directory("material-reflection");
    const auto path = (directory.Path() / "typed.mat").generic_string();
    asset::MaterialAsset original;
    original.params["uvTiling"] = {2, 3};
    original.integerParams["mask"] = {UINT32_MAX, 16777217};
    original.integerParams["mode"] = {-7};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, original));
    asset::MaterialAsset restored;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, restored));
    EXPECT_EQ(restored.integerParams.at("mask"), original.integerParams.at("mask"));
    EXPECT_EQ(restored.params.at("uvTiling"), original.params.at("uvTiling"));
    renderer::ShaderDescriptor descriptor;
    descriptor.vars = {Scalar("mode", renderer::ShaderVarType::Int, 0)};
    std::vector<uint8_t> bytes(4);
    asset::ApplyMaterialAssetParams(restored, descriptor, bytes);
    EXPECT_EQ(Read<int32_t>(bytes, 0), -7);
}

TEST_F(MaterialReflectionTest, DataAssetKeysAndAssetListsRoundTrip)
{
    ReflectionData original;
    original.key = input::KeyCode::TAB;
    original.textures.resize(2);
    original.textures[0].reference = {"texture-guid", "Assets/Texture.png"};
    toml::table table;
    util::TomlWriteReflector writer(table);
    original.Reflect(writer);
    ASSERT_TRUE(table["textures"].is_array());
    ReflectionData restored;
    util::TomlReadReflector reader(table);
    restored.Reflect(reader);
    EXPECT_EQ(restored.key, input::KeyCode::TAB);
    ASSERT_EQ(restored.textures.size(), 2u);
    EXPECT_EQ(restored.textures[0].reference.guid, "texture-guid");
    EXPECT_EQ(restored.textures[0].reference.path, "Assets/Texture.png");
    EXPECT_TRUE(restored.textures[1].reference.guid.empty());
}

TEST_F(MaterialReflectionTest, SolidDielectricTypedSettingsRoundTripAndRuntimeCopies)
{
    testkit::TempDir directory("material-dielectric");
    const auto path = (directory.Path() / "glass.mat").generic_string();
    asset::MaterialAsset original;
    original.dielectric.transmission = 1;
    original.dielectric.ior = 1.6f;
    original.dielectric.attenuationColor = {0, 0.5f, 0.75f};
    original.dielectric.attenuationDistance = 2.5f;
    original.dielectric.thinWalled = true;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, original));
    asset::MaterialAsset restored;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, restored));
    EXPECT_EQ(restored.dielectric, original.dielectric);
    renderer::Material runtime;
    runtime.dielectric = restored.dielectric;
    auto clone = runtime.CloneWithoutGpuResources();
    EXPECT_EQ(clone.dielectric, original.dielectric);
    renderer::Material moved(std::move(clone));
    EXPECT_EQ(moved.dielectric, original.dielectric);
    renderer::Material assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.dielectric, original.dielectric);
}

TEST_F(MaterialReflectionTest, LegacyAlphaDoesNotImplicitlyEnableOpticalTransmission)
{
    testkit::TempDir directory("material-legacy-alpha");
    const auto path = directory.Path() / "legacy.mat";
    {
        std::ofstream file(path);
        file << "blend_mode = 'AlphaBlend'\n[params]\nbaseColor = [1, 1, 1, 0.2]\n";
    }
    asset::MaterialAsset restored;
    restored.dielectric.transmission = 1;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path.generic_string(), restored));
    EXPECT_EQ(restored.blendMode, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_FLOAT_EQ(restored.params.at("baseColor")[3], 0.2f);
    EXPECT_EQ(restored.dielectric, renderer::SolidDielectricSettings{});
}

TEST_F(MaterialReflectionTest, RejectsWrongDielectricTypesWithoutModifyingOutput)
{
    testkit::TempDir directory("material-dielectric-invalid");
    const auto path = directory.Path() / "invalid.mat";
    asset::MaterialAsset preserved;
    preserved.shaderPath = "preserved";
    preserved.dielectric.transmission = 0.75f;
    const auto originalSettings = preserved.dielectric;
    for (const auto* invalid : {
        "dielectric = 'glass'\n", "[dielectric]\ntransmission = '1'\n",
        "[dielectric]\nior = true\n", "[dielectric]\nattenuation_distance = '1'\n",
        "[dielectric]\nthin_walled = 0\n", "[dielectric]\nattenuation_color = [1, 1]\n",
        "[dielectric]\nattenuation_color = [1, true, 1]\n",
        "[dielectric]\nior = 1e300\n", "[dielectric]\nior = -1e300\n",
        "[dielectric]\ntransmission = 1e300\n", "[dielectric]\nattenuation_distance = 1e300\n",
        "[dielectric]\nattenuation_color = [1e300, 1, 1]\n",
        "[dielectric]\nattenuation_color = [1, -1e300, 1]\n"}) {
        {
            std::ofstream file(path);
            file << invalid;
        }
        EXPECT_FALSE(asset::LoadMaterialAssetFromFile(path.generic_string(), preserved));
        EXPECT_EQ(preserved.shaderPath, "preserved");
        EXPECT_EQ(preserved.dielectric, originalSettings);
    }
    {
        std::ofstream file(path);
        file << "[dielectric]\ntransmission = 1\nior = 2\nattenuation_color = [0, 1, 1]\n";
    }
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path.generic_string(), preserved));
    EXPECT_FLOAT_EQ(preserved.dielectric.transmission, 1);
    EXPECT_FLOAT_EQ(preserved.dielectric.ior, 2);
    EXPECT_VEC3_NEAR(preserved.dielectric.attenuationColor, (math::Vector3{0, 1, 1}), 1e-6f);
    EXPECT_FLOAT_EQ(preserved.dielectric.attenuationDistance, 1);
    EXPECT_FALSE(preserved.dielectric.thinWalled);
}

TEST_F(MaterialReflectionTest, LegacyAssetPathsAndMissingFieldsRemainCompatible)
{
    toml::table table{{"texture", "Assets/Old.png"}};
    util::TomlReadReflector reader(table);
    scene::ScriptAssetReference reference;
    reader.AssetField("texture", reference, scene::ScriptAssetType::Texture);
    EXPECT_EQ(reference.path, "Assets/Old.png");
    std::vector<scene::ScriptAssetReference> list{{"keep", "Assets/Keep.png"}};
    reader.AssetListField("missing", list, scene::ScriptAssetType::Texture);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].guid, "keep");
}

TEST_F(MaterialReflectionTest, PolymorphicDataKeepsTypeAndNestedAssetReferences)
{
    ASSERT_TRUE(scene::ScriptSerializableFactory::Register("NestedReflectionData",
        [] { return std::make_unique<NestedReflectionData>(); }));
    ReflectionData original;
    ASSERT_TRUE(original.config.SetType("NestedReflectionData"));
    auto& config = static_cast<NestedReflectionData&>(*original.config.value);
    config.count = 23;
    config.textures.resize(1);
    config.textures[0].reference = {"nested-guid", "Assets/Nested.png"};
    toml::table table;
    util::TomlWriteReflector writer(table);
    original.Reflect(writer);
    ReflectionData restored;
    util::TomlReadReflector reader(table);
    restored.Reflect(reader);
    ASSERT_EQ(restored.config.type, "NestedReflectionData");
    ASSERT_NE(restored.config.value, nullptr);
    auto& result = static_cast<NestedReflectionData&>(*restored.config.value);
    EXPECT_EQ(result.count, 23);
    ASSERT_EQ(result.textures.size(), 1u);
    EXPECT_EQ(result.textures[0].reference.guid, "nested-guid");
}

TEST_F(MaterialReflectionTest, MissingPolymorphicTypePreservesFieldsAcrossSave)
{
    ReflectionData original;
    original.config.type = "UnavailableReflectionType";
    original.config.preservedFieldsToml = "count = 42\n";
    toml::table table;
    util::TomlWriteReflector writer(table);
    original.Reflect(writer);
    ReflectionData restored;
    util::TomlReadReflector reader(table);
    restored.Reflect(reader);
    EXPECT_EQ(restored.config.value, nullptr);
    EXPECT_EQ(restored.config.type, original.config.type);
    const auto parsed = toml::parse(restored.config.preservedFieldsToml);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed["count"].value_or(0), 42);
}

}
