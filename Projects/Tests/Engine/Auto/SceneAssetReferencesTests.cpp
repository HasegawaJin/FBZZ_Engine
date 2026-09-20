/// @file    SceneAssetReferencesTests.cpp
/// @brief   シーン本文から先読みすべきアセット参照を拾う規則を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/SceneAssetReferences.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using scene::CollectSceneAssetReferences;
using scene::SceneAssetKind;
using scene::SceneAssetReference;

class SceneAssetReferencesTest : public testkit::EngineFixture {};

} // namespace

TEST_F(SceneAssetReferencesTest, ClassifiesByExtension)
{
    const auto references = CollectSceneAssetReferences(R"(
[[objects.components]]
materialPath = "Assets/Materials/Rock.mat"
meshPath = "Assets/Models/Rock.fbx:2"
texture = 'Assets/Textures/Moss.PNG'
name = "Rock"
)");

    const std::vector<SceneAssetReference> expected = {
        { SceneAssetKind::Material, "Assets/Materials/Rock.mat" },
        { SceneAssetKind::Model, "Assets/Models/Rock.fbx" },
        { SceneAssetKind::Texture, "Assets/Textures/Moss.PNG" },
    };
    EXPECT_EQ(references, expected) << "submesh 添字を落とし、拡張子の大小を問わない";
}

TEST_F(SceneAssetReferencesTest, KeepsGuidReferencesAndUsesTheHintForTheKind)
{
    const auto references = CollectSceneAssetReferences(
        R"(model = "guid:0123456789abcdef0123456789abcdef|Assets/Models/Boss.fbx:0")");

    ASSERT_EQ(references.size(), 1u);
    EXPECT_EQ(references[0].kind, SceneAssetKind::Model);
    EXPECT_EQ(references[0].reference, "guid:0123456789abcdef0123456789abcdef|Assets/Models/Boss.fbx");
}

TEST_F(SceneAssetReferencesTest, ResolvesHintlessGuidThroughTheCallback)
{
    const auto references = CollectSceneAssetReferences(
        R"(tex = "guid:0123456789abcdef0123456789abcdef")",
        [](std::string_view) { return std::string("C:/Project/Assets/UI/Icon.png"); });

    ASSERT_EQ(references.size(), 1u);
    EXPECT_EQ(references[0].kind, SceneAssetKind::Texture);
    EXPECT_EQ(references[0].reference, "guid:0123456789abcdef0123456789abcdef");
}

TEST_F(SceneAssetReferencesTest, SpriteReferencePrefetchesTheParentTextureOnce)
{
    const auto references = CollectSceneAssetReferences(R"(
a = "Assets/UI/Atlas.png::sprite::Key_W"
b = "Assets/UI/Atlas.png::sprite::Key_S"
)");

    ASSERT_EQ(references.size(), 1u);
    EXPECT_EQ(references[0].reference, "Assets/UI/Atlas.png");
}

TEST_F(SceneAssetReferencesTest, IgnoresCommentsUnknownExtensionsAndAbsoluteDriveColons)
{
    const auto references = CollectSceneAssetReferences(R"(
# old = "Assets/Removed.png"
script = "PlayerComponent"
clip = "Assets/Anims/Run.anim"
tex = "C:/Project/Assets/Tex/A.dds"
)");

    ASSERT_EQ(references.size(), 1u);
    EXPECT_EQ(references[0].reference, "C:/Project/Assets/Tex/A.dds") << "ドライブ区切りを submesh 添字と取り違えない";
}

} // namespace fbzz::tests
