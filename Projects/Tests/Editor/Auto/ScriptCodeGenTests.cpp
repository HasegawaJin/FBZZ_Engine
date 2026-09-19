/// @file    ScriptCodeGenTests.cpp
/// @brief   Assets/Scripts を走査して ScriptList.inl の登録ブロックを差し替える処理。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// ここが誤ると «Inspector の Add Script に出ない» か «ビルドが通らない» のどちらかになる。
/// 特に共有基底とインターフェースを登録してしまうと make_unique<T>() が要求され、
/// 純粋仮想を持つ型でコンパイルごと落ちる。4 つのマクロの扱い分けを固定する。
///
/// 生成はマーカー行の «あいだ» だけを置き換える方式。人が書いた前後を巻き込まないこと、
/// マーカーが無いファイルを黙って壊さないことも、あわせて見る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/Util/ScriptCodeGen.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fbzz::tests {
namespace {

using editor::ScriptCodeGen;

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

class ScriptCodeGenTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::filesystem::create_directories(ScriptsDir());

        /// @note 生成はマーカーの «あいだ» を置換する方式。器が無いと何も書かれない。
        WriteFile(ListInl(),
                  "// 手書きの前書き\n"
                  "// @@FBZZ_SCRIPT_ENTRIES_BEGIN\n"
                  "// @@FBZZ_SCRIPT_ENTRIES_END\n"
                  "// 手書きの後書き\n");
        WriteFile(DllCpp(),
                  "// @@FBZZ_SCRIPT_INCLUDES_BEGIN\n"
                  "// @@FBZZ_SCRIPT_INCLUDES_END\n");
    }

    std::filesystem::path ScriptsDir() const { return m_temp.File("Scripts"); }
    std::filesystem::path HlslDir()    const { return m_temp.File("Shaders"); }
    std::filesystem::path DllCpp()     const { return m_temp.File("Dll.cpp"); }
    std::filesystem::path ListInl()    const { return ScriptsDir() / "ScriptList.inl"; }

    std::string ScriptsDirUtf8() const { return ScriptsDir().generic_string(); }
    std::string HlslDirUtf8()    const { return HlslDir().generic_string(); }
    std::string DllCppUtf8()     const { return DllCpp().generic_string(); }

    void AddHeader(const std::string& fileName, const std::string& body)
    {
        WriteFile(ScriptsDir() / fileName, body);
    }

    bool Sync() { return ScriptCodeGen::SyncScriptRegistry(ScriptsDirUtf8(), DllCppUtf8()); }

    std::string Registry() const { return ReadFile(ListInl()); }
    std::string Includes() const { return ReadFile(DllCpp()); }

private:
    testkit::TempDir m_temp{"scriptcodegen"};
};

/// @name 登録の対象と非対象

TEST_F(ScriptCodeGenTest, RegistersAPlainScript)
{
    AddHeader("PlayerComponent.hpp",
              "#pragma once\nclass PlayerComponent {\n  FBZZ_SCRIPT(PlayerComponent)\n};\n");

    ASSERT_TRUE(Sync());

    EXPECT_TRUE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(sandbox, PlayerComponent)")) << Registry();
}

TEST_F(ScriptCodeGenTest, RegistersADerivedScript)
{
    AddHeader("BossComponent.hpp",
              "#pragma once\nclass BossComponent {\n"
              "  FBZZ_SCRIPT_DERIVED(BossComponent, EnemyBase)\n};\n");

    ASSERT_TRUE(Sync());

    EXPECT_TRUE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(sandbox, BossComponent)")) << Registry();
}

TEST_F(ScriptCodeGenTest, DoesNotRegisterASharedBase)
{
    /// @note 基底を登録すると make_unique<T>() が要求され、純粋仮想を持つ型でビルドが落ちる。
    AddHeader("EnemyBase.hpp",
              "#pragma once\nclass EnemyBase {\n  FBZZ_SCRIPT_BASE(EnemyBase)\n};\n");

    ASSERT_TRUE(Sync());

    EXPECT_FALSE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(sandbox, EnemyBase)")) << Registry();
}

TEST_F(ScriptCodeGenTest, DoesNotRegisterAnInterface)
{
    AddHeader("IDamageable.hpp",
              "#pragma once\nclass IDamageable {\n  FBZZ_SCRIPT_INTERFACE(IDamageable)\n};\n");

    ASSERT_TRUE(Sync());

    EXPECT_FALSE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(sandbox, IDamageable)")) << Registry();
}

TEST_F(ScriptCodeGenTest, StillIncludesTheHeaderOfANonRegisteredType)
{
    /// @note 登録はしないが、派生の定義に要るのでヘッダは取り込まれなければならない。
    AddHeader("EnemyBase.hpp",
              "#pragma once\nclass EnemyBase {\n  FBZZ_SCRIPT_BASE(EnemyBase)\n};\n");

    ASSERT_TRUE(Sync());

    EXPECT_TRUE(Contains(Includes(), "EnemyBase.hpp")) << Includes();
}

TEST_F(ScriptCodeGenTest, IgnoresAHeaderWithoutAnyMacro)
{
    /// @note FBZZ_SCRIPT を持たないユーティリティは Add Script に出てはいけない。
    AddHeader("MathHelpers.hpp",
              "#pragma once\nstruct MathHelpers { static int Add(int,int); };\n");

    ASSERT_TRUE(Sync());

    EXPECT_FALSE(Contains(Registry(), "MathHelpers")) << Registry();
}

TEST_F(ScriptCodeGenTest, DistinguishesScriptFromItsPrefixedSiblings)
{
    /// @note "FBZZ_SCRIPT(" は "FBZZ_SCRIPT_BASE(" に一致してはいけない。
    ///       ここが混ざると基底まで登録され、ビルドが落ちる。
    AddHeader("Mixed.hpp",
              "#pragma once\n"
              "class Real {\n  FBZZ_SCRIPT(Real)\n};\n"
              "class Base {\n  FBZZ_SCRIPT_BASE(Base)\n};\n");

    ASSERT_TRUE(Sync());

    const std::string registry = Registry();
    EXPECT_TRUE(Contains(registry, "FBZZ_SCRIPT_ENTRY(sandbox, Real)")) << registry;
    EXPECT_FALSE(Contains(registry, "FBZZ_SCRIPT_ENTRY(sandbox, Base)")) << registry;
}

/// @name 名前空間

TEST_F(ScriptCodeGenTest, RecordsTheNamespaceOfAScript)
{
    AddHeader("PlayerComponent.hpp",
              "#pragma once\nnamespace game {\n"
              "class PlayerComponent {\n  FBZZ_SCRIPT(PlayerComponent)\n};\n"
              "}\n");

    ASSERT_TRUE(Sync());

    EXPECT_TRUE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(game, PlayerComponent)")) << Registry();
}

TEST_F(ScriptCodeGenTest, HandlesACompoundNamespaceOnOneLine)
{
    /// @note "namespace a::b {" は 1 行で読めるので、そのまま修飾名になる。
    AddHeader("PlayerComponent.hpp",
              "#pragma once\nnamespace game::player {\n"
              "class PlayerComponent {\n  FBZZ_SCRIPT(PlayerComponent)\n};\n"
              "}\n");

    ASSERT_TRUE(Sync());

    EXPECT_TRUE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(game::player, PlayerComponent)"))
        << Registry();
}

/// @name マーカーの扱い

TEST_F(ScriptCodeGenTest, KeepsTheHandWrittenPartsAroundTheMarkers)
{
    /// @note 生成はマーカーの «あいだ» だけ。人が書いた前後を巻き込んで消してはいけない。
    AddHeader("PlayerComponent.hpp",
              "#pragma once\nclass PlayerComponent {\n  FBZZ_SCRIPT(PlayerComponent)\n};\n");

    ASSERT_TRUE(Sync());

    const std::string registry = Registry();
    EXPECT_TRUE(Contains(registry, "手書きの前書き")) << registry;
    EXPECT_TRUE(Contains(registry, "手書きの後書き")) << registry;
}

TEST_F(ScriptCodeGenTest, ReportsFailureWhenTheMarkersAreMissing)
{
    /// @note マーカーの無いファイルを «全部書き換える» で処理すると、人の書いたものが消える。
    WriteFile(ListInl(), "// マーカーの無いファイル\n");
    AddHeader("PlayerComponent.hpp",
              "#pragma once\nclass PlayerComponent {\n  FBZZ_SCRIPT(PlayerComponent)\n};\n");

    EXPECT_FALSE(Sync());
    EXPECT_EQ(ReadFile(ListInl()), "// マーカーの無いファイル\n");
}

TEST_F(ScriptCodeGenTest, ClearsTheBlockWhenEveryScriptIsGone)
{
    /// @note スクリプトを消したのに登録が残ると、存在しない型を new しに行く。
    AddHeader("PlayerComponent.hpp",
              "#pragma once\nclass PlayerComponent {\n  FBZZ_SCRIPT(PlayerComponent)\n};\n");
    ASSERT_TRUE(Sync());
    ASSERT_TRUE(Contains(Registry(), "PlayerComponent"));

    std::filesystem::remove(ScriptsDir() / "PlayerComponent.hpp");
    ASSERT_TRUE(Sync());

    EXPECT_FALSE(Contains(Registry(), "PlayerComponent")) << Registry();
}

/// @name 出力の性質

TEST_F(ScriptCodeGenTest, ProducesTheSameFileForTheSameInput)
{
    /// @note 走査順で並びが揺れると、何も変えていないのに毎回 git の差分が出る。
    AddHeader("Alpha.hpp", "#pragma once\nclass Alpha {\n  FBZZ_SCRIPT(Alpha)\n};\n");
    AddHeader("Beta.hpp",  "#pragma once\nclass Beta {\n  FBZZ_SCRIPT(Beta)\n};\n");

    ASSERT_TRUE(Sync());
    const std::string first = Registry();
    ASSERT_TRUE(Sync());

    EXPECT_EQ(Registry(), first);
}

TEST_F(ScriptCodeGenTest, ListsEveryScriptItFinds)
{
    AddHeader("Alpha.hpp", "#pragma once\nclass Alpha {\n  FBZZ_SCRIPT(Alpha)\n};\n");
    AddHeader("Beta.hpp",  "#pragma once\nclass Beta {\n  FBZZ_SCRIPT(Beta)\n};\n");

    ASSERT_TRUE(Sync());

    const std::string registry = Registry();
    EXPECT_TRUE(Contains(registry, "FBZZ_SCRIPT_ENTRY(sandbox, Alpha)")) << registry;
    EXPECT_TRUE(Contains(registry, "FBZZ_SCRIPT_ENTRY(sandbox, Beta)")) << registry;
}

TEST_F(ScriptCodeGenTest, FindsScriptsInSubdirectories)
{
    AddHeader("Combat/BossComponent.hpp",
              "#pragma once\nclass BossComponent {\n  FBZZ_SCRIPT(BossComponent)\n};\n");

    ASSERT_TRUE(Sync());

    EXPECT_TRUE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(sandbox, BossComponent)")) << Registry();
}

TEST_F(ScriptCodeGenTest, DoesNotListTheSameScriptTwice)
{
    AddHeader("PlayerComponent.hpp",
              "#pragma once\nclass PlayerComponent {\n  FBZZ_SCRIPT(PlayerComponent)\n};\n");

    ASSERT_TRUE(Sync());

    const std::string registry = Registry();
    const std::string entry = "FBZZ_SCRIPT_ENTRY(sandbox, PlayerComponent)";
    EXPECT_EQ(registry.find(entry), registry.rfind(entry)) << registry;
}

TEST_F(ScriptCodeGenTest, RejectsAnEmptyScriptsDirectoryArgument)
{
    EXPECT_FALSE(ScriptCodeGen::SyncScriptRegistry("", DllCppUtf8()));
}

/// @name スクリプト雛形の生成

TEST_F(ScriptCodeGenTest, CreateScriptAppendsTheComponentSuffixForBehaviours)
{
    /// @note アタッチするスクリプトの慣習。ここがぶれると Add Script の一覧で名前が揃わない。
    const std::string created =
        ScriptCodeGen::CreateScript("My", ScriptsDirUtf8(), DllCppUtf8());

    ASSERT_FALSE(created.empty());
    EXPECT_TRUE(Contains(created, "MyComponent.hpp")) << created;
    EXPECT_TRUE(Contains(ReadFile(std::filesystem::path(created)),
                         "FBZZ_SCRIPT(MyComponent)"));
}

TEST_F(ScriptCodeGenTest, CreateScriptForAUtilityKeepsTheNameAndOmitsTheMacro)
{
    /// @note ユーティリティはアタッチしない。Component を付けると意味が逆になる。
    const std::string created =
        ScriptCodeGen::CreateScript("MyHelpers", ScriptsDirUtf8(), DllCppUtf8(), {},
                                    ScriptCodeGen::ScriptKind::Utility);

    ASSERT_FALSE(created.empty());
    EXPECT_TRUE(Contains(created, "MyHelpers.hpp")) << created;
    EXPECT_FALSE(Contains(ReadFile(std::filesystem::path(created)), "FBZZ_SCRIPT("));
}

TEST_F(ScriptCodeGenTest, CreateScriptForADataAssetUsesTheDataAssetMacro)
{
    const std::string created =
        ScriptCodeGen::CreateScript("MyTuning", ScriptsDirUtf8(), DllCppUtf8(), {},
                                    ScriptCodeGen::ScriptKind::DataAsset);

    ASSERT_FALSE(created.empty());
    EXPECT_TRUE(Contains(ReadFile(std::filesystem::path(created)), "FBZZ_DATA_ASSET("));
}

TEST_F(ScriptCodeGenTest, CreateScriptRefusesToOverwriteAnExistingFile)
{
    ASSERT_FALSE(ScriptCodeGen::CreateScript("My", ScriptsDirUtf8(), DllCppUtf8()).empty());

    /// @note 2 回目は既存を上書きせず失敗する (人の書いたコードを消さない)。
    EXPECT_TRUE(ScriptCodeGen::CreateScript("My", ScriptsDirUtf8(), DllCppUtf8()).empty());
}

TEST_F(ScriptCodeGenTest, CreateScriptRejectsEmptyArguments)
{
    EXPECT_TRUE(ScriptCodeGen::CreateScript("", ScriptsDirUtf8(), DllCppUtf8()).empty());
    EXPECT_TRUE(ScriptCodeGen::CreateScript("My", "", DllCppUtf8()).empty());
}

TEST_F(ScriptCodeGenTest, CreatedScriptIsPickedUpByTheRegistry)
{
    ASSERT_FALSE(ScriptCodeGen::CreateScript("My", ScriptsDirUtf8(), DllCppUtf8()).empty());

    ASSERT_TRUE(Sync());

    EXPECT_TRUE(Contains(Registry(), "FBZZ_SCRIPT_ENTRY(sandbox, MyComponent)")) << Registry();
}

/// @name HLSL 雛形

TEST_F(ScriptCodeGenTest, CreateHlslWritesAFileForEveryKind)
{
    const std::pair<ScriptCodeGen::HlslKind, const char*> kinds[] = {
        { ScriptCodeGen::HlslKind::SurfaceVSPS,     "SurfaceKind" },
        { ScriptCodeGen::HlslKind::PostProcessVSPS, "PostKind" },
        { ScriptCodeGen::HlslKind::ComputeCS,       "ComputeKind" },
        { ScriptCodeGen::HlslKind::ParticlePS,      "ParticleKind" },
    };
    for (const auto& [kind, name] : kinds) {
        const std::string created = ScriptCodeGen::CreateHlsl(name, HlslDirUtf8(), kind);
        ASSERT_FALSE(created.empty()) << name;
        EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(created))) << created;
    }
}

TEST_F(ScriptCodeGenTest, ParticleShaderIncludesTheParticleMaterialHelpers)
{
    /// @note ParticleEmitter 用の PS は ParticleMaterial.hlsli を通さないと、
    ///       生成した .hlsl がそのままではコンパイルできない。
    const std::string created = ScriptCodeGen::CreateHlsl(
        "MySpark", HlslDirUtf8(), ScriptCodeGen::HlslKind::ParticlePS);

    ASSERT_FALSE(created.empty());
    EXPECT_TRUE(Contains(ReadFile(std::filesystem::path(created)),
                         "ParticleMaterial.hlsli"));
}

TEST_F(ScriptCodeGenTest, CreateHlslRejectsEmptyArguments)
{
    EXPECT_TRUE(ScriptCodeGen::CreateHlsl("", HlslDirUtf8(),
                                          ScriptCodeGen::HlslKind::SurfaceVSPS).empty());
    EXPECT_TRUE(ScriptCodeGen::CreateHlsl("X", "",
                                          ScriptCodeGen::HlslKind::SurfaceVSPS).empty());
}

} // namespace fbzz::tests
