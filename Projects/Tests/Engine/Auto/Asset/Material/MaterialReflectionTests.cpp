/// @file    MaterialReflectionTests.cpp
/// @brief   Material の数値精度・HLSL 配置と DataAsset の型付き保存を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/Material/MaterialParamBinding.hpp>
#include <Engine/Asset/Data/DataAsset.hpp>
#include <Engine/Asset/Data/DataAssetFactory.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/RenderSettings/PostProcessProfile.hpp>
#include <Engine/Asset/RenderSettings/RenderPipelineAsset.hpp>
#include <Engine/Scene/Components/Geometry/MaterialComponent.hpp>
#include <bit>
#include <Graphics/Renderer/Materials/Material.hpp>
#include <Engine/Scene/Serialization/TomlReflector.hpp>
#include <array>
#include <cstring>
#include <filesystem>
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

class CheckedAssetBase : public asset::DataAsset {
public:
    static constexpr const char* TYPE_NAME = "CheckedAssetBase";
    const char* GetTypeName() const override { return TYPE_NAME; }
    void Reflect(scene::IReflector&) override {}
    int baseValue = 41;
};

class CheckedAssetImpostor final : public asset::DataAsset {
public:
    const char* GetTypeName() const override { return CheckedAssetBase::TYPE_NAME; }
    void Reflect(scene::IReflector&) override {}
};

class PipelineAssetImpostor final : public asset::DataAsset {
public:
    const char* GetTypeName() const override { return asset::RenderPipelineAsset::TYPE_NAME; }
    void Reflect(scene::IReflector&) override {}
};

struct CheckedAssetPadding {
    virtual ~CheckedAssetPadding() = default;
    int padding = 73;
};

class CheckedAssetDerived final : public CheckedAssetPadding, public virtual CheckedAssetBase {
public:
    static constexpr const char* TYPE_NAME = "CheckedAssetDerived";
    const char* GetTypeName() const override { return TYPE_NAME; }
    int derivedValue = 97;
};

class NestedReflectionData final : public scene::IScriptSerializable {
    FBZZ_SERIALIZABLE(NestedReflectionData)
    FBZZ_FIELD(int, count, 0, "Count")
    FBZZ_ASSET_LIST_FIELD(scene::TextureRef, textures, "Textures")
};
FBZZ_REFLECT(NestedReflectionData)

}

class MaterialReflectionTest : public testkit::EngineFixture {};

TEST_F(MaterialReflectionTest, TypedDataAssetRejectsImpostorWithSameSerializedName)
{
    ASSERT_TRUE(asset::DataAssetFactory::Register(CheckedAssetBase::TYPE_NAME,
        [] { return std::make_unique<CheckedAssetImpostor>(); }));
    testkit::TempDir directory("typed-data-asset-impostor");
    asset::Asset<CheckedAssetBase> reference;
    reference.ref.path = directory.File("impostor.fzdata").generic_string();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(reference.ref.path, CheckedAssetBase::TYPE_NAME));
    ASSERT_NE(asset::DataAssetRegistry::Resolve(reference.ref.path), nullptr);
    EXPECT_EQ(asset::DataAssetRegistry::TypeOf(reference.ref.path), CheckedAssetBase::TYPE_NAME);
    EXPECT_EQ(reference.Get(), nullptr);
}

TEST_F(MaterialReflectionTest, TypedDataAssetReturnsCompatibleBaseSubobject)
{
    ASSERT_TRUE((asset::DataAssetFactory::Register<CheckedAssetDerived, CheckedAssetBase>()));
    testkit::TempDir directory("typed-data-asset-derived");
    asset::Asset<CheckedAssetBase> base;
    base.ref.path = directory.File("derived.fzdata").generic_string();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(base.ref.path, CheckedAssetDerived::TYPE_NAME));
    asset::Asset<CheckedAssetDerived> derived;
    derived.ref.path = base.ref.path;
    ASSERT_NE(derived.Get(), nullptr);
    ASSERT_NE(base.Get(), nullptr);
    EXPECT_EQ(base.Get(), static_cast<CheckedAssetBase*>(derived.Get()));
    EXPECT_EQ(base->baseValue, 41);
    EXPECT_EQ(derived->derivedValue, 97);
    ASSERT_TRUE(asset::DataAssetFactory::Register(CheckedAssetDerived::TYPE_NAME,
        [] { return std::make_unique<CheckedAssetImpostor>(); }));
    EXPECT_EQ(base.Get(), static_cast<CheckedAssetBase*>(derived.Get()));
}

TEST_F(MaterialReflectionTest, TypedDataAssetAcceptsExactLegacyTypeWithoutLineage)
{
    ASSERT_TRUE(asset::DataAssetFactory::Register(ReflectionData::TYPE_NAME,
        [] { return std::make_unique<ReflectionData>(); }));
    testkit::TempDir directory("typed-data-asset-legacy");
    asset::Asset<ReflectionData> reference;
    reference.ref.path = directory.File("legacy.fzdata").generic_string();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(reference.ref.path, ReflectionData::TYPE_NAME));
    ASSERT_NE(reference.Get(), nullptr);
    EXPECT_EQ(reference->key, input::KeyCode::SPACE);
}

TEST_F(MaterialReflectionTest, TypedDataAssetResolvesBuiltinAcrossEngineDll)
{
    testkit::TempDir directory("typed-data-asset-builtin");
    asset::Asset<asset::PostProcessProfile> reference;
    reference.ref.path = directory.File("look.fzdata").generic_string();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(reference.ref.path, asset::PostProcessProfile::TYPE_NAME));
    ASSERT_NE(reference.Get(), nullptr);
    EXPECT_EQ(static_cast<asset::DataAsset*>(reference.Get()), asset::DataAssetRegistry::Resolve(reference.ref.path));
    EXPECT_TRUE(reference->overrides.empty());
}

TEST_F(MaterialReflectionTest, DataAssetRegistryRejectsPipelineCodecImpostor)
{
    constexpr auto TYPE_NAME = "PipelineCodecImpostor";
    ASSERT_TRUE(asset::DataAssetFactory::Register(TYPE_NAME, [] { return std::make_unique<PipelineAssetImpostor>(); }));
    testkit::TempDir directory("typed-data-asset-pipeline-impostor");
    const auto path = directory.File("impostor.fzdata");
    EXPECT_FALSE(asset::DataAssetRegistry::Create(path.generic_string(), TYPE_NAME));
    EXPECT_FALSE(std::filesystem::exists(path));
}

/// @brief 葉の風変形を明示しない旧形式の材質は変形しない。
TEST_F(MaterialReflectionTest, LegacyMaterialsStayRigidUntilLeafWindIsExplicitlyAuthored)
{
    renderer::ShaderDescriptor descriptor;
    descriptor.cbufferSize = 96;
    descriptor.vars = {
        Scalar("leafWindStrength", renderer::ShaderVarType::Float, 68),
        Scalar("leafWindFrequency", renderer::ShaderVarType::Float, 72),
        Scalar("leafWindRootHeight", renderer::ShaderVarType::Float, 76),
        Scalar("customMultiplier", renderer::ShaderVarType::Float, 12)
    };
    std::vector<uint8_t> bytes(descriptor.cbufferSize);
    asset::MaterialAssetAuthoring bark;
    bark.params["roughness"] = {0.92f};
    asset::InitDefaultMaterialParams(descriptor, bytes);
    asset::ApplyMaterialAssetParams(bark, descriptor, bytes);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 68), 0.0f);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 72), 1.0f);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 76), 0.0f);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 12), 1.0f);

    asset::MaterialAssetAuthoring leaves;
    leaves.params["leafWindStrength"] = {0.13f};
    leaves.params["leafWindFrequency"] = {1.4f};
    leaves.params["leafWindRootHeight"] = {1.8f};
    asset::ApplyMaterialAssetParams(leaves, descriptor, bytes);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 68), 0.13f);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 72), 1.4f);
    EXPECT_FLOAT_EQ(Read<float>(bytes, 76), 1.8f);
}

/// @brief 整数と bool の上書きを浮動小数点の丸めなしで GPU のバイト列へ渡す。
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

/// @brief スカラー配列の 16 バイトストライドとパディングを保つ。
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

/// @brief 行列配列の行優先・列優先の両方に対応する。
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

/// @brief 不正値や出力バッファーの不足時は書き込み前の値をすべて保つ。
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

/// @brief 材質ファイルは符号なし整数の上限値と旧形式の浮動小数点パラメーターを保持する。
TEST_F(MaterialReflectionTest, MaterialFilePreservesUnsignedLimitsAndLegacyFloatParams)
{
    testkit::TempDir directory("material-reflection");
    const auto path = (directory.Path() / "typed.mat").generic_string();
    asset::MaterialAssetAuthoring original;
    original.params["uvTiling"] = {2, 3};
    original.integerParams["mask"] = {UINT32_MAX, 16777217};
    original.integerParams["mode"] = {-7};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, original));
    asset::MaterialAssetAuthoring restored;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, restored));
    EXPECT_EQ(restored.integerParams.at("mask"), original.integerParams.at("mask"));
    EXPECT_EQ(restored.params.at("uvTiling"), original.params.at("uvTiling"));
    renderer::ShaderDescriptor descriptor;
    descriptor.vars = {Scalar("mode", renderer::ShaderVarType::Int, 0)};
    std::vector<uint8_t> bytes(4);
    asset::ApplyMaterialAssetParams(restored, descriptor, bytes);
    EXPECT_EQ(Read<int32_t>(bytes, 0), -7);
}

/// @brief DataAsset のキーとアセット参照の配列を保存して読み戻す。
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

/// @brief ソリッド誘電体の型付き設定を保存して読み戻し、実行時の値へコピーする。
TEST_F(MaterialReflectionTest, SolidDielectricTypedSettingsRoundTripAndRuntimeCopies)
{
    testkit::TempDir directory("material-dielectric");
    const auto path = (directory.Path() / "glass.mat").generic_string();
    asset::MaterialAssetAuthoring original;
    original.dielectric.transmission = 1;
    original.dielectric.ior = 1.6f;
    original.dielectric.attenuationColor = {0, 0.5f, 0.75f};
    original.dielectric.attenuationDistance = 2.5f;
    original.dielectric.thinWalled = true;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, original));
    asset::MaterialAssetAuthoring restored;
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

/// @brief 旧形式のアルファ設定で光学透過が暗黙に有効にならない。
TEST_F(MaterialReflectionTest, LegacyAlphaDoesNotImplicitlyEnableOpticalTransmission)
{
    testkit::TempDir directory("material-legacy-alpha");
    const auto path = directory.Path() / "legacy.mat";
    {
        std::ofstream file(path);
        file << "blend_mode = 'AlphaBlend'\n[params]\nbaseColor = [1, 1, 1, 0.2]\n";
    }
    asset::MaterialAssetAuthoring restored;
    restored.dielectric.transmission = 1;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path.generic_string(), restored));
    EXPECT_EQ(restored.blendMode, renderer::BlendMode::ALPHA_BLEND);
    EXPECT_FLOAT_EQ(restored.params.at("baseColor")[3], 0.2f);
    EXPECT_EQ(restored.dielectric, renderer::SolidDielectricSettings{});
}

/// @brief 誘電体設定の型が不正なら出力を変更せず拒否する。
TEST_F(MaterialReflectionTest, RejectsWrongDielectricTypesWithoutModifyingOutput)
{
    testkit::TempDir directory("material-dielectric-invalid");
    const auto path = directory.Path() / "invalid.mat";
    asset::MaterialAssetAuthoring preserved;
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

/// @brief 旧形式のアセットパスと項目の欠落を受け付ける。
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

/// @brief 多態データの型と入れ子のアセット参照を保持する。
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

/// @brief 多態データの型が未登録でも保存時に項目を保持する。
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

/// @brief 下書きへの参照の保持や編集の競合で、公開済みの編集値を変更できない。
TEST_F(MaterialReflectionTest, RetainedDraftAliasesAndConflictingEditsCannotMutatePublishedAuthoring)
{
    asset::MaterialAsset material;
    const auto initial = material.ReadAuthoring();
    auto first = material.BeginAuthoringEdit();
    auto stale = material.BeginAuthoringEdit();
    auto& retained = first.Draft().params["roughness"];
    retained = {0.4f};
    ASSERT_EQ(first.Commit(material), asset::detail::MaterialAuthoringCommitResult::COMMITTED);
    const auto published = material.ReadAuthoring();
    retained[0] = 0.9f;
    EXPECT_TRUE(initial->Data().params.empty());
    EXPECT_FLOAT_EQ(published->Data().params.at("roughness")[0], 0.4f);
    stale.Draft().shaderPath = "stale.hlsl";
    EXPECT_EQ(stale.Commit(material), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
    EXPECT_EQ(material.ReadAuthoring(), published);
    auto cancelled = material.BeginAuthoringEdit();
    cancelled.Draft().textures["albedo"] = "discarded.png";
    cancelled.Cancel();
    EXPECT_EQ(cancelled.Commit(material), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
    EXPECT_EQ(material.ReadAuthoring(), published);
}

/// @brief 編集値が同じなら公開版を維持し、浮動小数点のビット差は変更とみなす。
TEST_F(MaterialReflectionTest, ExactAuthoringEqualityPreservesUnchangedIdentityAndDistinguishesFloatBits)
{
    asset::MaterialAsset material;
    auto edit = material.BeginAuthoringEdit();
    edit.Draft().params["bits"] = {0.0f, std::bit_cast<float>(uint32_t{0x7FC00001})};
    ASSERT_EQ(edit.Commit(material), asset::detail::MaterialAuthoringCommitResult::COMMITTED);
    const auto first = material.ReadAuthoring();
    auto equal = material.BeginAuthoringEdit();
    EXPECT_EQ(equal.Commit(material), asset::detail::MaterialAuthoringCommitResult::UNCHANGED);
    EXPECT_EQ(material.ReadAuthoring(), first);
    auto negativeZero = material.BeginAuthoringEdit();
    negativeZero.Draft().params["bits"][0] = -0.0f;
    ASSERT_EQ(negativeZero.Commit(material), asset::detail::MaterialAuthoringCommitResult::COMMITTED);
    const auto second = material.ReadAuthoring();
    EXPECT_NE(second, first);
    EXPECT_EQ(std::bit_cast<uint32_t>(first->Data().params.at("bits")[0]), 0u);
    EXPECT_EQ(std::bit_cast<uint32_t>(second->Data().params.at("bits")[0]), 0x80000000u);
    auto otherNaN = material.BeginAuthoringEdit();
    otherNaN.Draft().params["bits"][1] = std::bit_cast<float>(uint32_t{0x7FC00002});
    EXPECT_EQ(otherNaN.Commit(material), asset::detail::MaterialAuthoringCommitResult::COMMITTED);
    EXPECT_EQ(std::bit_cast<uint32_t>(second->Data().params.at("bits")[1]), 0x7FC00001u);
}

/// @brief スロット配列を置換した場合も、表示の代替値を現行の編集値から取得する。
TEST_F(MaterialReflectionTest, LiveVisibilityFallbackAndSlotVectorReplacementResolveCurrentAuthoringOwners)
{
    scene::MaterialComponent material;
    material.SetMaterialPath("base.mat");
    material.extraSlots.resize(1);
    const auto base = material.ReadAuthoring();
    const auto extra = material.RawSlotAt(1).ReadAuthoring();
    EXPECT_EQ(material.SlotAt(1).ReadAuthoring(), base);
    material.extraSlots[0].visible = false;
    EXPECT_EQ(material.SlotAt(1).ReadAuthoring(), extra);
    material.extraSlots[0].visible = true;
    EXPECT_EQ(material.SlotAt(1).ReadAuthoring(), base);
    material.extraSlots[0].SetMaterialPath("extra.mat");
    const auto selected = material.SlotAt(1).ReadAuthoring();
    scene::MaterialSlot replacement;
    replacement.SetMaterialPath("replacement.mat");
    material.extraSlots[0] = replacement;
    EXPECT_NE(material.SlotAt(1).ReadAuthoring()->OwnerToken(), selected->OwnerToken());
    EXPECT_NE(material.SlotAt(1).ReadAuthoring()->OwnerToken(), replacement.ReadAuthoring()->OwnerToken());
    EXPECT_EQ(selected->Data().materialPath, "extra.mat");
    const auto replaced = material.SlotAt(1).ReadAuthoring();
    material.extraSlots.reserve(material.extraSlots.capacity() + 8);
    EXPECT_EQ(material.SlotAt(1).ReadAuthoring(), replaced);
    material.enabled = false;
    EXPECT_EQ(material.SlotAt(1).ReadAuthoring(), replaced);
    material.extraSlots.clear();
    EXPECT_EQ(material.SlotAt(1).ReadAuthoring(), base);
}

/// @brief 同一ハンドルでのアセット置換とコピー・ムーブは、それぞれの公開版の所有権を正しく扱う。
TEST_F(MaterialReflectionTest, SameHandleAssetReplacementAndCopyMoveHaveDistinctOwnedPublications)
{
    asset::MaterialAssetAuthoring values;
    values.params["roughness"] = {0.2f};
    auto& store = asset::AssetStore<asset::MaterialAsset>::Get();
    const auto handle = store.Alloc(std::make_unique<asset::MaterialAsset>(values));
    const auto first = store.GetPtr(handle)->ReadAuthoring();
    values.params["roughness"] = {0.8f};
    ASSERT_TRUE(store.Replace(handle, std::make_unique<asset::MaterialAsset>(values)));
    const auto second = store.GetPtr(handle)->ReadAuthoring();
    EXPECT_NE(first->OwnerToken(), second->OwnerToken());
    EXPECT_FLOAT_EQ(first->Data().params.at("roughness")[0], 0.2f);
    EXPECT_FLOAT_EQ(second->Data().params.at("roughness")[0], 0.8f);
    asset::MaterialAsset copy(*store.GetPtr(handle));
    EXPECT_NE(copy.ReadAuthoring()->OwnerToken(), second->OwnerToken());
    const auto copied = copy.ReadAuthoring();
    asset::MaterialAsset moved(std::move(copy));
    EXPECT_EQ(moved.ReadAuthoring(), copied);
    EXPECT_EQ(copy.ReadAuthoring(), nullptr);
    store.Free(handle);
}

/// @brief 全値を置換する取り消しでは値をコピーし、読み込みの失敗時は所有者のスナップショットを保つ。
TEST_F(MaterialReflectionTest, WholeValueUndoPublicationCopiesValuesAndFailedLoadPreservesOwnerSnapshot)
{
    asset::MaterialAssetAuthoring values;
    values.params["roughness"] = {0.5f};
    asset::MaterialAsset material(values);
    const auto saved = material.ReadAuthoring();
    material.ReplaceAuthoring(values);
    const auto replaced = material.ReadAuthoring();
    values.params["roughness"][0] = 0.9f;
    EXPECT_FLOAT_EQ(replaced->Data().params.at("roughness")[0], 0.5f);
    EXPECT_EQ(saved->OwnerToken(), replaced->OwnerToken());
    EXPECT_NE(saved, replaced);
    testkit::TempDir directory("material-owned-load");
    const auto path = directory.File("invalid.mat").generic_string();
    { std::ofstream file(path); file << "[dielectric]\nior = true\n"; }
    EXPECT_FALSE(asset::LoadMaterialAssetFromFile(path, material));
    EXPECT_EQ(material.ReadAuthoring(), replaced);
    EXPECT_FALSE(asset::LoadMaterialAssetFromFile(path, values));
    EXPECT_FLOAT_EQ(values.params.at("roughness")[0], 0.9f);
}


}
