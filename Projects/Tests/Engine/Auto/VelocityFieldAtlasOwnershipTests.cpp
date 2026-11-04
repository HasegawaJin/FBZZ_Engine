/// @file    VelocityFieldAtlasOwnershipTests.cpp
/// @brief   速度場アトラスの作成元分離、退役資源と終了時の返却を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Asset/VelocityFieldAtlas.hpp>
#include <Engine/Core/Time.hpp>
#include <Fluid/VectorFieldAsset.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>

namespace fbzz::tests {
class VelocityFieldAtlasOwnershipTest : public testkit::EngineFixture {
protected:
    void SetUp() override { EngineFixture::SetUp(); m_frame = Time::frameCount; asset::VelocityFieldAtlas::Reset(); }
    void TearDown() override { asset::VelocityFieldAtlas::Reset(); Time::frameCount = m_frame; EngineFixture::TearDown(); }
    fluid::VectorFieldAsset MakeField()
    {
        fluid::VectorFieldAsset field;
        field.sizeX = field.sizeY = field.sizeZ = fluid::kVelocityFieldTileResolution;
        field.data.resize(static_cast<size_t>(field.sizeX) * field.sizeY * field.sizeZ);
        return field;
    }
    uint64_t m_frame = 0;
};

TEST_F(VelocityFieldAtlasOwnershipTest, AlternatingManagersReuseTheirAtlasAndResetReturnsAllTextures)
{
    testkit::ResourceTrackingRenderer firstDevice, secondDevice;
    renderer::ResourceManager first(firstDevice), second(secondDevice);
    const auto field = MakeField(), next = MakeField();
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, first), 0);
    const auto retired = asset::VelocityFieldAtlas::Texture(first);
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(next, first), 1);
    const auto initial = asset::VelocityFieldAtlas::Texture(first);
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, second), 0);
    const auto other = asset::VelocityFieldAtlas::Texture(second);

    EXPECT_EQ(asset::VelocityFieldAtlas::Acquire(next, first), 1);
    EXPECT_EQ(asset::VelocityFieldAtlas::Texture(first), initial);
    EXPECT_EQ(asset::VelocityFieldAtlas::Texture(second), other);
    EXPECT_NE(first.Get(retired), nullptr);
    EXPECT_EQ(firstDevice.m_texturesCreated, 2u);
    asset::VelocityFieldAtlas::Reset();

    EXPECT_EQ(firstDevice.m_texturesAlive, 0u);
    EXPECT_EQ(secondDevice.m_texturesAlive, 0u);
    EXPECT_EQ(first.Get(initial), nullptr);
    EXPECT_EQ(second.Get(other), nullptr);
}

TEST_F(VelocityFieldAtlasOwnershipTest, InvalidationRebuildsEveryOwnerAndRetiresThroughTheOriginalManager)
{
    testkit::ResourceTrackingRenderer firstDevice, secondDevice;
    renderer::ResourceManager first(firstDevice), second(secondDevice);
    const auto field = MakeField();
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, first), 0);
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, second), 0);
    const auto oldFirst = asset::VelocityFieldAtlas::Texture(first);
    const auto oldSecond = asset::VelocityFieldAtlas::Texture(second);

    asset::VelocityFieldAtlas::Invalidate(first);
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, first), 0);
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, second), 0);
    EXPECT_NE(asset::VelocityFieldAtlas::Texture(first), oldFirst);
    EXPECT_NE(asset::VelocityFieldAtlas::Texture(second), oldSecond);
    EXPECT_NE(first.Get(oldFirst), nullptr);
    EXPECT_NE(second.Get(oldSecond), nullptr);
    Time::frameCount += 3;
    EXPECT_EQ(asset::VelocityFieldAtlas::Acquire(field, first), 0);
    EXPECT_EQ(asset::VelocityFieldAtlas::Acquire(field, second), 0);

    EXPECT_EQ(first.Get(oldFirst), nullptr);
    EXPECT_EQ(second.Get(oldSecond), nullptr);
    EXPECT_EQ(firstDevice.m_texturesAlive, 1u);
    EXPECT_EQ(secondDevice.m_texturesAlive, 1u);
    asset::VelocityFieldAtlas::Reset();
}

TEST_F(VelocityFieldAtlasOwnershipTest, ResetAndEarlyOwnerDestructionPreserveTheOtherOwner)
{
    testkit::ResourceTrackingRenderer firstDevice, secondDevice;
    auto first = std::make_unique<renderer::ResourceManager>(firstDevice);
    renderer::ResourceManager second(secondDevice);
    const auto field = MakeField();
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, *first), 0);
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, second), 0);
    const auto other = asset::VelocityFieldAtlas::Texture(second);
    const auto stale = asset::VelocityFieldAtlas::Texture(*first);

    first->Reset();
    const auto unrelated = first->CreateTexture3D(nullptr, 1, 1, 1);
    ASSERT_EQ(asset::VelocityFieldAtlas::Acquire(field, *first), 0);
    EXPECT_EQ(first->Get(stale), nullptr);
    EXPECT_NE(first->Get(unrelated), nullptr);
    EXPECT_EQ(asset::VelocityFieldAtlas::Texture(second), other);
    first.reset();
    EXPECT_EQ(asset::VelocityFieldAtlas::Acquire(field, second), 0);
    asset::VelocityFieldAtlas::Reset();

    EXPECT_EQ(firstDevice.m_texturesAlive, 0u);
    EXPECT_EQ(secondDevice.m_texturesAlive, 0u);
}

TEST_F(VelocityFieldAtlasOwnershipTest, UnsupportedOwnerDoesNotDisableAnotherBackendAndFallbackIsReturned)
{
    testkit::ResourceTrackingRenderer unsupportedDevice, supportedDevice;
    unsupportedDevice.m_supportTexture3D = false;
    renderer::ResourceManager unsupported(unsupportedDevice), supported(supportedDevice);
    const auto field = MakeField();
    EXPECT_EQ(asset::VelocityFieldAtlas::Acquire(field, unsupported), -1);
    EXPECT_FALSE(asset::VelocityFieldAtlas::Texture(unsupported).IsValid());
    const auto fallback = asset::VelocityFieldAtlas::Texture(supported);
    ASSERT_NE(supported.Get(fallback), nullptr);
    EXPECT_EQ(supported.Get(fallback)->GetDepth(), 1u);
    EXPECT_EQ(asset::VelocityFieldAtlas::Acquire(field, supported), 0);
    asset::VelocityFieldAtlas::Reset();
    EXPECT_EQ(supportedDevice.m_texturesAlive, 0u);
}
} /// @note namespace fbzz::tests
