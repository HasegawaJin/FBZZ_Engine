/// @file    ManagedScriptMetadataTests.cpp
/// @brief   Script 宣言の checksum と .meta の型 GUID の移行・欠損保持を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Editor/Ai/Json.hpp>
#include <Editor/Ai/JsonReflector.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Util/FileSystem.hpp>
#include "../../../Editor/src/ManagedScriptMetadata.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

constexpr char ABC_HASH[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr char BOM_ABC_HASH[] = "1c28dc3f1f804a1ad9c9b4b4cf5e2658d16ad4ed08e3020d04a8d2865018947c";
constexpr char TYPE_GUID[] = "11111111111111111111111111111111";

editor::ai::JsonValue Declaration(const char* path, const char* hash = ABC_HASH)
{
    editor::ai::JsonValue source = editor::ai::JsonValue::MakeObject();
    source.Set("path", editor::ai::JsonValue(path));
    source.Set("line", editor::ai::JsonValue(1));
    source.Set("contentHash", editor::ai::JsonValue(hash));
    return source;
}

editor::ai::JsonValue Type(const char* name, const std::vector<editor::ai::JsonValue>& declarations)
{
    editor::ai::JsonValue type = editor::ai::JsonValue::MakeObject();
    type.Set("clrName", editor::ai::JsonValue(name));
    editor::ai::JsonValue sources = editor::ai::JsonValue::MakeArray();
    for (const auto& declaration : declarations) sources.Push(declaration);
    type.Set("sources", std::move(sources));
    return type;
}

std::string Metadata(const char* name, const std::string& guid = TYPE_GUID, const char* previous = nullptr)
{
    std::string text = "[meta]\nguid = '22222222222222222222222222222222'\n"
        "[custom]\nkeep = 'untouched'\n[[managed.types]]\nguid = '" + guid + "'\nclrName = '" + name + "'\n";
    if (previous) text += std::string("previousClrNames = ['") + previous + "']\n";
    return text;
}

} /// @note namespace

class ManagedScriptMetadataTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(m_temp.File("Assets/Scripts")));
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(m_temp.File("Library/Candidate")));
    }

    void Write(const std::string& relative, const std::string& contents)
    {
        ASSERT_TRUE(util::FileSystem::WriteText(m_temp.File(relative), contents));
    }

    std::string Read(const std::string& relative)
    {
        std::string value;
        EXPECT_TRUE(util::FileSystem::ReadText(m_temp.File(relative), value));
        return value;
    }

    bool Prepare(const std::vector<editor::ai::JsonValue>& types)
    {
        editor::ai::JsonValue catalog = editor::ai::JsonValue::MakeArray();
        for (const auto& type : types) catalog.Push(type);
        return editor::PrepareManagedScriptManifest(m_temp.Path(), Assembly(),
            editor::ai::SerializeJson(catalog), m_diagnostic);
    }

    std::filesystem::path Assembly() const { return m_temp.File("Library/Candidate/Test.Scripts.dll"); }

    editor::ai::JsonValue Manifest()
    {
        const auto parsed = editor::ai::ParseJson(Read("Library/Candidate/Test.Scripts.dll.scripts.json"));
        EXPECT_TRUE(parsed.has_value());
        return parsed.value_or(editor::ai::JsonValue{});
    }

    testkit::TempDir m_temp{ "managed-metadata" };
    std::string m_diagnostic;
};

TEST_F(ManagedScriptMetadataTest, SeparateTypesKeepStableIdsAndUnknownMetadata)
{
    Write("Assets/Scripts/Actors.cs", "abc");
    Write("Assets/Scripts/Actors.cs.meta", "[meta]\nguid = '22222222222222222222222222222222'\n[custom]\nkeep = 'untouched'\n");
    const std::vector types{ Type("Actor", { Declaration("Assets/Scripts/Actors.cs") }),
        Type("Health", { Declaration("Assets/Scripts/Actors.cs") }) };
    ASSERT_TRUE(Prepare(types)) << m_diagnostic;
    const std::string firstManifest = Read("Library/Candidate/Test.Scripts.dll.scripts.json");
    const auto manifest = Manifest();
    ASSERT_NE(manifest.Find("types"), nullptr);
    const auto& records = manifest.Find("types")->AsArray();
    ASSERT_EQ(records.size(), 2u);
    ASSERT_NE(records[0].Find("id"), nullptr);
    ASSERT_NE(records[1].Find("id"), nullptr);
    EXPECT_NE(records[0].Find("id")->AsString(), records[1].Find("id")->AsString());
    EXPECT_TRUE(records[0].Find("id")->AsString().starts_with("cs:"));
    EXPECT_NE(Read("Assets/Scripts/Actors.cs.meta").find("untouched"), std::string::npos);
    ASSERT_TRUE(Prepare(types)) << m_diagnostic;
    EXPECT_EQ(Read("Library/Candidate/Test.Scripts.dll.scripts.json"), firstManifest);
}

TEST_F(ManagedScriptMetadataTest, PartialRequiresOneExplicitPrimaryAndDoesNotGuessFirstFile)
{
    Write("Assets/Scripts/A.cs", "abc");
    Write("Assets/Scripts/Z.cs", "abc");
    const std::vector types{ Type("PartialActor", { Declaration("Assets/Scripts/A.cs"), Declaration("Assets/Scripts/Z.cs") }) };
    EXPECT_FALSE(Prepare(types));
    EXPECT_NE(m_diagnostic.find("explicit primary"), std::string::npos);
    EXPECT_FALSE(util::FileSystem::Exists(m_temp.File("Assets/Scripts/A.cs.meta")));
    EXPECT_FALSE(util::FileSystem::Exists(m_temp.File("Assets/Scripts/Z.cs.meta")));
    Write("Assets/Scripts/Z.cs.meta", Metadata("PartialActor"));
    ASSERT_TRUE(Prepare(types)) << m_diagnostic;
    const auto manifest = Manifest();
    const auto& record = manifest.Find("types")->AsArray()[0];
    EXPECT_EQ(record.Find("id")->AsString(), std::string("cs:") + TYPE_GUID);
    EXPECT_EQ(record.Find("primarySource")->AsString(), "Assets/Scripts/Z.cs");
}

TEST_F(ManagedScriptMetadataTest, DuplicatePartialMappingsAreRejectedWithoutOverwritingFiles)
{
    Write("Assets/Scripts/A.cs", "abc");
    Write("Assets/Scripts/Z.cs", "abc");
    const std::string first = Metadata("PartialActor");
    const std::string second = Metadata("PartialActor", "33333333333333333333333333333333");
    Write("Assets/Scripts/A.cs.meta", first);
    Write("Assets/Scripts/Z.cs.meta", second);
    EXPECT_FALSE(Prepare({ Type("PartialActor", { Declaration("Assets/Scripts/A.cs"), Declaration("Assets/Scripts/Z.cs") }) }));
    EXPECT_EQ(Read("Assets/Scripts/A.cs.meta"), first);
    EXPECT_EQ(Read("Assets/Scripts/Z.cs.meta"), second);
}

TEST_F(ManagedScriptMetadataTest, ExplicitRenameAndOrphanRestoreKeepTypeId)
{
    Write("Assets/Scripts/Actor.cs", "abc");
    Write("Assets/Scripts/Actor.cs.meta", Metadata("OldActor"));
    ASSERT_TRUE(Prepare({ Type("OldActor", { Declaration("Assets/Scripts/Actor.cs") }) })) << m_diagnostic;
    Write("Assets/Scripts/Actor.cs.meta", Metadata("NewActor", TYPE_GUID, "OldActor"));
    ASSERT_TRUE(Prepare({ Type("NewActor", { Declaration("Assets/Scripts/Actor.cs") }) })) << m_diagnostic;
    EXPECT_EQ(Manifest().Find("types")->AsArray()[0].Find("id")->AsString(), std::string("cs:") + TYPE_GUID);
    const std::string metadata = Read("Assets/Scripts/Actor.cs.meta");
    std::error_code error;
    ASSERT_TRUE(std::filesystem::remove(m_temp.File("Assets/Scripts/Actor.cs"), error));
    ASSERT_FALSE(error);
    ASSERT_TRUE(Prepare({})) << m_diagnostic;
    EXPECT_EQ(Read("Assets/Scripts/Actor.cs.meta"), metadata);
    EXPECT_EQ(scene::ScriptFactory::DisplayName(std::string("cs:") + TYPE_GUID), "NewActor");
    Write("Assets/Scripts/Actor.cs", "abc");
    ASSERT_TRUE(Prepare({ Type("NewActor", { Declaration("Assets/Scripts/Actor.cs") }) })) << m_diagnostic;
    EXPECT_EQ(Manifest().Find("types")->AsArray()[0].Find("id")->AsString(), std::string("cs:") + TYPE_GUID);
}

TEST_F(ManagedScriptMetadataTest, SeedsDisplayNameForUnregisteredManagedType)
{
    const std::string typeKey = std::string("cs:") + TYPE_GUID;
    Write("Assets/Scripts/Actor.cs", "abc");
    Write("Assets/Scripts/Actor.cs.meta", Metadata("CandidateActor"));
    ASSERT_TRUE(Prepare({ Type("CandidateActor", { Declaration("Assets/Scripts/Actor.cs") }) })) << m_diagnostic;
    EXPECT_EQ(Manifest().Find("types")->AsArray()[0].Find("clrName")->AsString(), "CandidateActor");
    ASSERT_TRUE(editor::LoadManagedScriptDisplayNames(m_temp.Path(), m_diagnostic)) << m_diagnostic;
    EXPECT_EQ(scene::ScriptFactory::DisplayName(typeKey), "CandidateActor");
}

TEST_F(ManagedScriptMetadataTest, StaleChecksumAndMalformedMetadataPreserveExistingCandidate)
{
    Write("Assets/Scripts/Actor.cs", "abc");
    Write("Assets/Scripts/Actor.cs.meta", Metadata("Actor"));
    ASSERT_TRUE(Prepare({ Type("Actor", { Declaration("Assets/Scripts/Actor.cs") }) })) << m_diagnostic;
    const std::string manifest = Read("Library/Candidate/Test.Scripts.dll.scripts.json");
    const std::string original = Read("Assets/Scripts/Actor.cs.meta");
    EXPECT_FALSE(Prepare({ Type("Actor", { Declaration("Assets/Scripts/Actor.cs", BOM_ABC_HASH) }) }));
    EXPECT_EQ(Read("Assets/Scripts/Actor.cs.meta"), original);
    EXPECT_EQ(Read("Library/Candidate/Test.Scripts.dll.scripts.json"), manifest);
    Write("Assets/Scripts/Actor.cs.meta", "[meta\n");
    EXPECT_FALSE(Prepare({ Type("Actor", { Declaration("Assets/Scripts/Actor.cs") }) }));
    EXPECT_EQ(Read("Assets/Scripts/Actor.cs.meta"), "[meta\n");
    EXPECT_EQ(Read("Library/Candidate/Test.Scripts.dll.scripts.json"), manifest);
}

TEST_F(ManagedScriptMetadataTest, Utf8BomIsPartOfTheCompiledChecksum)
{
    Write("Assets/Scripts/Actor.cs", "\xEF\xBB\xBF" "abc");
    ASSERT_TRUE(Prepare({ Type("Actor", { Declaration("Assets/Scripts/Actor.cs", BOM_ABC_HASH) }) })) << m_diagnostic;
    EXPECT_FALSE(Prepare({ Type("Actor", { Declaration("Assets/Scripts/Actor.cs", ABC_HASH) }) }));
    EXPECT_NE(m_diagnostic.find("checksum differs"), std::string::npos);
}

TEST_F(ManagedScriptMetadataTest, SourceOutsideAssetsCannotCreateMetadata)
{
    EXPECT_FALSE(Prepare({ Type("Actor", { Declaration("Assets/../Actor.cs") }) }));
    EXPECT_FALSE(util::FileSystem::Exists(m_temp.File("Actor.cs.meta")));
}

TEST_F(ManagedScriptMetadataTest, RenameHistoryCannotAmbiguouslyOwnAnotherTypeName)
{
    Write("Assets/Scripts/Actor.cs", "abc");
    const std::string metadata = Metadata("NewActor", TYPE_GUID, "OldActor");
    Write("Assets/Scripts/Actor.cs.meta", metadata);
    EXPECT_FALSE(Prepare({ Type("OldActor", { Declaration("Assets/Scripts/Actor.cs") }) }));
    EXPECT_EQ(Read("Assets/Scripts/Actor.cs.meta"), metadata);
    EXPECT_NE(m_diagnostic.find("Ambiguous"), std::string::npos);
}

TEST_F(ManagedScriptMetadataTest, UppercaseSourceExtensionKeepsExistingTypeId)
{
    Write("Assets/Scripts/Actor.CS", "abc");
    Write("Assets/Scripts/Actor.CS.meta", Metadata("Actor"));
    ASSERT_TRUE(Prepare({ Type("Actor", { Declaration("Assets/Scripts/Actor.CS") }) })) << m_diagnostic;
    EXPECT_EQ(Manifest().Find("types")->AsArray()[0].Find("id")->AsString(), std::string("cs:") + TYPE_GUID);
}

TEST_F(ManagedScriptMetadataTest, InvalidSourceCatalogCannotReplacePreparedSidecar)
{
    Write("Assets/Scripts/Actor.cs", "abc");
    Write("Assets/Scripts/Actor.cs.meta", Metadata("Actor"));
    ASSERT_TRUE(Prepare({ Type("Actor", { Declaration("Assets/Scripts/Actor.cs") }) })) << m_diagnostic;
    const std::string manifest = Read("Library/Candidate/Test.Scripts.dll.scripts.json");
    const std::string metadata = Read("Assets/Scripts/Actor.cs.meta");
    EXPECT_FALSE(editor::PrepareManagedScriptManifest(m_temp.Path(), Assembly(), "{", m_diagnostic));
    EXPECT_EQ(Read("Library/Candidate/Test.Scripts.dll.scripts.json"), manifest);
    EXPECT_EQ(Read("Assets/Scripts/Actor.cs.meta"), metadata);
}

TEST_F(ManagedScriptMetadataTest, ObjectReferenceWritePreservesSceneIdentifiersAndNullSlots)
{
    editor::ai::JsonValue reference = editor::ai::JsonValue::MakeObject();
    reference.Set("guid", editor::ai::JsonValue("11111111-1111-4111-8111-111111111111"));
    editor::ai::JsonValue array = editor::ai::JsonValue::MakeArray();
    array.Push(reference);
    array.Push(editor::ai::JsonValue{});
    array.Push(reference);
    std::vector<scene::ScriptObjectReference> values;
    editor::ai::JsonWriteReflector writer("targets", array);
    writer.ObjectReferenceListField("targets", values);
    ASSERT_TRUE(writer.Applied()) << writer.Error();
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0].guid, "11111111-1111-4111-8111-111111111111");
    EXPECT_TRUE(values[1].guid.empty());
    EXPECT_EQ(values[2].guid, values[0].guid);
    EXPECT_EQ(values[0].id, scene::EntityID::INVALID);
}

TEST_F(ManagedScriptMetadataTest, PrimitiveArraySchemaIncludesActualDefaultValues)
{
    std::vector<float> weights{ 1.0f, 2.0f };
    std::vector<bool> toggles{ true, false };
    editor::ai::JsonCatalogReflector schema;
    schema.ListField("weights", weights);
    schema.ListField("toggles", toggles);
    const auto& fields = schema.Result().AsArray();
    ASSERT_EQ(fields.size(), 2u);
    ASSERT_NE(fields[0].Find("default"), nullptr);
    ASSERT_EQ(fields[0].Find("default")->AsArray().size(), 2u);
    EXPECT_EQ(fields[0].Find("default")->AsArray()[1].AsNumber(), 2.0);
    ASSERT_NE(fields[1].Find("default"), nullptr);
    EXPECT_TRUE(fields[1].Find("default")->AsArray()[0].AsBool());
    EXPECT_FALSE(fields[1].Find("default")->AsArray()[1].AsBool());
}

} /// @note namespace fbzz::tests
