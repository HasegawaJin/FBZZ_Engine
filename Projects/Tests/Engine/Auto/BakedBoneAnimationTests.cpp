/// @file    BakedBoneAnimationTests.cpp
/// @brief   Bone bake clocks, quaternion interpolation, bounds, cache and corruption contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <TestKit/TestKit.hpp>
#include "../../../Engine/src/Scene/Systems/BoneCbBatch.hpp"
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <cstring>
#include <functional>
#include "../../../Engine/src/Scene/Systems/BonePoseSharing.hpp"
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Asset/BakedBoneAnimation.hpp>
#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace fbzz::tests {
namespace {
using BoneBakeTestStatus = asset::BoneBakeStatus;
asset::Skeleton MakeBoneBakeTestRig()
{
    asset::Skeleton s; s.rootNodeIndex = 0; s.nodes.resize(3); s.bones.resize(2);
    s.nodes[0].name = "Root"; s.nodes[0].children = {1};
    for (int i = 1; i < 3; ++i) {
        auto& n = s.nodes[static_cast<size_t>(i)]; n.name = i == 1 ? "Body" : "Tail";
        n.parentIndex = i - 1; n.boneIndex = i - 1; n.bindTranslation = {1.0f, 0.0f, 0.0f};
        n.localBindTransform = math::Matrix4::TRS(n.bindTranslation, n.bindRotation, n.bindScale);
        auto& b = s.bones[static_cast<size_t>(i - 1)]; b.name = n.name; b.nodeIndex = i;
        b.offsetMatrix = math::Matrix4::TRS({-static_cast<float>(i), 0.0f, 0.0f}, math::Quaternion::Identity(), math::Vector3::ONE);
    }
    s.nodes[1].children = {2}; return s;
}
asset::AnimationClip MakeBoneBakeTestSwim()
{
    asset::AnimationClip c; c.name = "MakeBoneBakeTestSwim"; c.durationSeconds = 2.0; c.durationTicks = 2.0; c.ticksPerSecond = 1.0; c.loop = true;
    asset::NodeAnimationTrack t; t.nodeName = "Body";
    t.positions = {{0.0, {1.0f, 0.0f, 0.0f}}, {1.0, {1.0f, 0.2f, 0.0f}}, {2.0, {1.0f, 0.0f, 0.0f}}};
    c.tracks.push_back(t); t = {}; t.nodeName = "Tail";
    t.rotations = {{0.0, math::Quaternion::Identity()}, {1.0, math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.2f)}, {2.0, math::Quaternion::Identity()}};
    c.tracks.push_back(t); return c;
}
asset::BakedBoneAnimation MakeBoneBakeTestAsset(const asset::Skeleton& rig = MakeBoneBakeTestRig(), const asset::AnimationClip& clip = MakeBoneBakeTestSwim())
{
    asset::BakedBoneAnimation b; EXPECT_EQ(asset::BakeBoneAnimation(rig, clip, {60.0f, 3.0f}, b), BoneBakeTestStatus::OK); return b;
}
asset::BakedBoneFrame MakeBoneBakeTestFrame(const asset::BakedBoneAnimation& b, double seconds, bool loop = true)
{
    asset::BakedBoneFrame f; EXPECT_EQ(asset::ResolveBakedBoneFrame(b, seconds, 1.0, 0.0, loop, f), BoneBakeTestStatus::OK); return f;
}
void ExpectBoneBakeTestMatrix(const math::Matrix4& a, const math::Matrix4& b, float epsilon = 0.0001f)
{
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) EXPECT_NEAR(a.m[r][c], b.m[r][c], epsilon);
}
std::vector<math::Matrix4> SampleBoneBakeTestReference(const asset::Skeleton& s, const asset::AnimationClip& c, double seconds)
{
    std::vector<math::Matrix4> g(s.nodes.size()), p(s.bones.size());
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        const auto& n = s.nodes[i]; auto t = n.bindTranslation; auto q = n.bindRotation; auto scale = n.bindScale;
        for (const auto& track : c.tracks) if (track.nodeName == n.name) {
            t = asset::SampleVectorKeys(track.positions, seconds, t); q = asset::SampleQuaternionKeys(track.rotations, seconds, q);
            scale = asset::SampleVectorKeys(track.scales, seconds, scale);
        }
        const auto local = math::Matrix4::TRS(t, q, scale);
        g[i] = n.parentIndex < 0 ? local : g[static_cast<size_t>(n.parentIndex)] * local;
    }
    for (size_t i = 0; i < p.size(); ++i) p[i] = s.rootInverseTransform * g[static_cast<size_t>(s.bones[i].nodeIndex)] * s.bones[i].offsetMatrix;
    return p;
}
void RepairBoneBakeTestChecksum(std::vector<std::byte>& bytes)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < bytes.size() - 8; ++i) h = (h ^ std::to_integer<uint8_t>(bytes[i])) * 1099511628211ull;
    for (int i = 0; i < 8; ++i) bytes[bytes.size() - 8 + i] = static_cast<std::byte>((h >> (8 * i)) & 255);
}
class BoneBakeTestCpuRenderer : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t,uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t,uint32_t,uint32_t,uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*,size_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*,uint32_t,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&,uint32_t,renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t,uint32_t,const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*,uint32_t,uint32_t) override { return {}; }
};
}
class BakedBoneAnimationTest : public testkit::EngineFixture {};

TEST_F(BakedBoneAnimationTest, PreservesEndpointsAndTheOrdinaryLocalTrsPalette)
{
    const auto s = MakeBoneBakeTestRig(); const auto c = MakeBoneBakeTestSwim(); const auto b = MakeBoneBakeTestAsset(s, c);
    ASSERT_EQ(b.frameCount, 121u);
    for (double seconds : {0.0, 0.125, 0.75, 1.0, 1.5, 2.0}) {
        std::vector<math::Matrix4> p, g;
        ASSERT_EQ(asset::EvaluateBakedBoneAnimation(b, MakeBoneBakeTestFrame(b, seconds, false), p, g), BoneBakeTestStatus::OK);
        const auto reference = SampleBoneBakeTestReference(s, c, seconds);
        ASSERT_EQ(p.size(), reference.size()); for (size_t i = 0; i < p.size(); ++i) ExpectBoneBakeTestMatrix(p[i], reference[i]);
    }
}

TEST_F(BakedBoneAnimationTest, LoopSeamUsesTheEndpointAndWrapsContinuously)
{
    const auto b = MakeBoneBakeTestAsset(); std::vector<math::Matrix4> before, after, g;
    const auto last = MakeBoneBakeTestFrame(b, 2.0 - 0.000001); ASSERT_EQ(last.second, b.frameCount - 1);
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(b, last, before, g), BoneBakeTestStatus::OK);
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(b, MakeBoneBakeTestFrame(b, 2.0 + 0.000001), after, g), BoneBakeTestStatus::OK);
    for (size_t i = 0; i < before.size(); ++i) ExpectBoneBakeTestMatrix(before[i], after[i]);
    EXPECT_EQ(MakeBoneBakeTestFrame(b, 2.0).first, 0u);
    EXPECT_EQ(MakeBoneBakeTestFrame(b, 2.0, false).first, b.frameCount - 1);
}

TEST_F(BakedBoneAnimationTest, IndependentPhaseSpeedReverseAndPausedClocks)
{
    const auto b = MakeBoneBakeTestAsset(); asset::BakedBoneFrame f;
    ASSERT_EQ(asset::ResolveBakedBoneFrame(b, 0.25, 2.0, 0.25, true, f), BoneBakeTestStatus::OK); EXPECT_EQ(f.first, 45u);
    ASSERT_EQ(asset::ResolveBakedBoneFrame(b, 0.25, -1.0, 0.0, true, f), BoneBakeTestStatus::OK); EXPECT_EQ(f.first, 105u);
    ASSERT_EQ(asset::ResolveBakedBoneFrame(b, 99.0, 0.0, 0.75, true, f), BoneBakeTestStatus::OK); EXPECT_EQ(f.first, 45u);
    EXPECT_EQ(MakeBoneBakeTestFrame(b, -100.0, false).first, 0u); EXPECT_EQ(MakeBoneBakeTestFrame(b, 100.0, false).second, b.frameCount - 1);
}

TEST_F(BakedBoneAnimationTest, ThousandIndependentFishMatchTheKeySampler)
{
    const auto s = MakeBoneBakeTestRig(); const auto c = MakeBoneBakeTestSwim(); const auto b = MakeBoneBakeTestAsset(s, c);
    for (uint32_t i = 0; i < 1000; ++i) {
        const double phase = static_cast<double>(i % 121) / 60.0;
        const double speed = 0.5 + static_cast<double>(i % 7) * 0.25;
        asset::BakedBoneFrame f; std::vector<math::Matrix4> p, g;
        ASSERT_EQ(asset::ResolveBakedBoneFrame(b, 0.5, speed, phase, true, f), BoneBakeTestStatus::OK);
        ASSERT_EQ(asset::EvaluateBakedBoneAnimation(b, f, p, g), BoneBakeTestStatus::OK);
        const auto reference = SampleBoneBakeTestReference(s, c, std::fmod(0.5 * speed + phase, 2.0));
        for (size_t bone = 0; bone < p.size(); ++bone) ExpectBoneBakeTestMatrix(p[bone], reference[bone]);
    }
}

TEST_F(BakedBoneAnimationTest, AntipodalQuaternionsDoNotTakeTheLongArc)
{
    auto c = MakeBoneBakeTestSwim(); c.tracks[1].rotations = {{0.0, {0.0f, 0.0f, 0.0f, 1.0f}}, {2.0, {0.0f, 0.0f, 0.0f, -1.0f}}};
    const auto b = MakeBoneBakeTestAsset(MakeBoneBakeTestRig(), c); std::vector<math::Matrix4> p, g;
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(b, MakeBoneBakeTestFrame(b, 0.9), p, g), BoneBakeTestStatus::OK);
    ExpectBoneBakeTestMatrix(p[1], SampleBoneBakeTestReference(MakeBoneBakeTestRig(), c, 0.9)[1]); EXPECT_NEAR(g[2].m[0][0], 1.0f, 0.00001f);
}

TEST_F(BakedBoneAnimationTest, BlendsLocalRotationsAndTranslationsBeforeHierarchyComposition)
{
    auto c = MakeBoneBakeTestSwim(); c.tracks[0].positions = {{0.0, {2.0f, 0.0f, 0.0f}}};
    c.tracks[1].rotations = {{0.0, math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.2f)}};
    const auto a = MakeBoneBakeTestAsset(), b = MakeBoneBakeTestAsset(MakeBoneBakeTestRig(), c); const auto f = MakeBoneBakeTestFrame(a, 0.0), tf = MakeBoneBakeTestFrame(b, 0.0);
    std::vector<math::Matrix4> p, g;
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(a, f, p, g, &b, &tf, 0.5f), BoneBakeTestStatus::OK);
    const auto expectedBody = math::Matrix4::TRS({1.5f, 0.0f, 0.0f}, math::Quaternion::Identity(), math::Vector3::ONE);
    const auto expectedTail = expectedBody * math::Matrix4::TRS({1.0f, 0.0f, 0.0f}, math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.6f), math::Vector3::ONE);
    ExpectBoneBakeTestMatrix(g[1], expectedBody); ExpectBoneBakeTestMatrix(g[2], expectedTail);
    float radius = 0.0f; EXPECT_EQ(asset::BoneBakeBlendRadius(a, b, radius), BoneBakeTestStatus::OK);
    EXPECT_GE(radius, a.conservativeRadius); EXPECT_GE(radius, b.conservativeRadius);
}

TEST_F(BakedBoneAnimationTest, BoundsContainRotatingScaledBonesAndEveryAllowedLod)
{
    auto c = MakeBoneBakeTestSwim(); c.tracks[0].scales = {{0.0, {1.0f, 2.0f, 0.5f}}, {2.0, {2.0f, 0.5f, 1.0f}}};
    const auto s = MakeBoneBakeTestRig(); const auto b = MakeBoneBakeTestAsset(s, c);
    for (uint32_t i = 0; i < 81; ++i) {
        std::vector<math::Matrix4> p, g; ASSERT_EQ(asset::EvaluateBakedBoneAnimation(b, MakeBoneBakeTestFrame(b, i / 40.0, false), p, g), BoneBakeTestStatus::OK);
        for (const auto& matrix : p) for (const auto& v : {math::Vector4{3.0f, 0.0f, 0.0f, 1.0f}, math::Vector4{0.0f, -3.0f, 0.0f, 1.0f}, math::Vector4{0.0f, 0.0f, 3.0f, 1.0f}}) {
            const auto transformed = matrix * v;
            EXPECT_LE(math::Vector3(transformed.x, transformed.y, transformed.z).Length(), b.conservativeRadius);
        }
    }
    EXPECT_EQ(asset::ValidateBoneBakeSource(b, s, c, 3.0f), BoneBakeTestStatus::OK);
    EXPECT_EQ(asset::ValidateBoneBakeSource(b, s, c, 3.01f), BoneBakeTestStatus::INCOMPATIBLE_LOD);
}

TEST_F(BakedBoneAnimationTest, ReimportedKeysAndBindingsInvalidateTheCache)
{
    const auto b = MakeBoneBakeTestAsset(); auto s = MakeBoneBakeTestRig(); auto c = MakeBoneBakeTestSwim();
    c.tracks[0].positions[1].value.y += 0.1f;
    EXPECT_EQ(asset::ValidateBoneBakeSource(b, s, c, 3.0f), BoneBakeTestStatus::STALE_SOURCE);
    c = MakeBoneBakeTestSwim(); s.bones[0].offsetMatrix.m[0][3] += 0.1f;
    EXPECT_EQ(asset::ValidateBoneBakeSource(b, s, c, 3.0f), BoneBakeTestStatus::INCOMPATIBLE_SKELETON);
    s = MakeBoneBakeTestRig(); std::swap(s.bones[0], s.bones[1]);
    EXPECT_EQ(asset::ValidateBoneBakeSource(b, s, c, 3.0f), BoneBakeTestStatus::INCOMPATIBLE_SKELETON);
}

TEST_F(BakedBoneAnimationTest, RejectsCycleChildrenMismatchAndBoneMismatch)
{
    auto s = MakeBoneBakeTestRig(); asset::BakedBoneAnimation output; s.nodes[1].parentIndex = 2;
    EXPECT_EQ(asset::BakeBoneAnimation(s, MakeBoneBakeTestSwim(), {60.0f, 3.0f}, output), BoneBakeTestStatus::INVALID_SKELETON);
    s = MakeBoneBakeTestRig(); s.nodes[0].children.clear(); EXPECT_EQ(asset::BakeBoneAnimation(s, MakeBoneBakeTestSwim(), {}, output), BoneBakeTestStatus::INVALID_SKELETON);
    s = MakeBoneBakeTestRig(); s.bones[0].nodeIndex = 2; EXPECT_EQ(asset::BakeBoneAnimation(s, MakeBoneBakeTestSwim(), {}, output), BoneBakeTestStatus::INVALID_SKELETON);
}

TEST_F(BakedBoneAnimationTest, UnsupportedTracksAndSideEffectsFallBackWithoutPublishing)
{
    auto c = MakeBoneBakeTestSwim(); asset::BakedBoneAnimation out; out.clipFingerprint = 42;
    c.hasRootMotion = true; EXPECT_EQ(asset::BakeBoneAnimation(MakeBoneBakeTestRig(), c, {}, out), BoneBakeTestStatus::UNSUPPORTED_SIDE_EFFECT);
    c = MakeBoneBakeTestSwim(); c.propertyTracks.emplace_back(); EXPECT_EQ(asset::BakeBoneAnimation(MakeBoneBakeTestRig(), c, {}, out), BoneBakeTestStatus::UNSUPPORTED_SIDE_EFFECT);
    c = MakeBoneBakeTestSwim(); c.tracks[0].interp = asset::AnimInterp::Step; EXPECT_EQ(asset::BakeBoneAnimation(MakeBoneBakeTestRig(), c, {}, out), BoneBakeTestStatus::UNSUPPORTED_TRACK);
    c = MakeBoneBakeTestSwim(); c.tracks[0].nodeName = "Unknown"; EXPECT_EQ(asset::BakeBoneAnimation(MakeBoneBakeTestRig(), c, {}, out), BoneBakeTestStatus::UNSUPPORTED_TRACK);
    EXPECT_EQ(out.clipFingerprint, 42u);
}

TEST_F(BakedBoneAnimationTest, RejectsBadKeyOrderNonFiniteClocksAndCapacityOverflow)
{
    auto c = MakeBoneBakeTestSwim(); asset::BakedBoneAnimation out; c.tracks[0].positions[1].time = 0.0;
    EXPECT_EQ(asset::BakeBoneAnimation(MakeBoneBakeTestRig(), c, {}, out), BoneBakeTestStatus::INVALID_CLIP);
    c = MakeBoneBakeTestSwim(); c.tracks[1].rotations[1].value.w = (std::numeric_limits<float>::quiet_NaN)();
    EXPECT_EQ(asset::BakeBoneAnimation(MakeBoneBakeTestRig(), c, {}, out), BoneBakeTestStatus::INVALID_CLIP);
    c = MakeBoneBakeTestSwim(); c.durationSeconds = c.durationTicks = 1.0e100;
    EXPECT_EQ(asset::BakeBoneAnimation(MakeBoneBakeTestRig(), c, {}, out), BoneBakeTestStatus::CAPACITY_EXCEEDED);
    const auto b = MakeBoneBakeTestAsset(); asset::BakedBoneFrame f{7, 8, 0.25f, 0};
    EXPECT_EQ(asset::ResolveBakedBoneFrame(b, (std::numeric_limits<double>::infinity)(), 1.0, 0.0, true, f), BoneBakeTestStatus::INVALID_DATA);
    EXPECT_EQ(f.first, 7u);
}

TEST_F(BakedBoneAnimationTest, InvalidFrameAndBlendKeepThePublishedPalette)
{
    const auto b = MakeBoneBakeTestAsset(); std::vector<math::Matrix4> p{math::Matrix4::Identity()}, g = p;
    EXPECT_EQ(asset::EvaluateBakedBoneAnimation(b, {b.frameCount, b.frameCount, 0.0f, 0}, p, g), BoneBakeTestStatus::INVALID_DATA);
    EXPECT_EQ(p.size(), 1u);
    auto other = b; other.nodes[2].parent = 0; const auto f = MakeBoneBakeTestFrame(b, 0.0);
    EXPECT_EQ(asset::EvaluateBakedBoneAnimation(b, f, p, g, &other, &f, 0.5f), BoneBakeTestStatus::INCOMPATIBLE_SKELETON);
    EXPECT_EQ(p.size(), 1u);
}

TEST_F(BakedBoneAnimationTest, ReusedWorkspaceKeepsFailureAtomicAcrossClipChanges)
{
    const auto bake = MakeBoneBakeTestAsset();
    asset::BakedBoneWorkspace workspace;
    std::vector<math::Matrix4> matrices, globals;
    const auto first = MakeBoneBakeTestFrame(bake, 0.25);
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(bake, first, matrices, globals, nullptr, nullptr, 0, &workspace), BoneBakeTestStatus::OK);
    const auto published = matrices;
    auto invalid = first; invalid.first = bake.frameCount;
    EXPECT_EQ(asset::EvaluateBakedBoneAnimation(bake, invalid, matrices, globals, nullptr, nullptr, 0, &workspace), BoneBakeTestStatus::INVALID_DATA);
    for (size_t i = 0; i < matrices.size(); ++i) ExpectBoneBakeTestMatrix(matrices[i], published[i]);
    const auto second = MakeBoneBakeTestFrame(bake, 0.75);
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(bake, second, matrices, globals, nullptr, nullptr, 0, &workspace), BoneBakeTestStatus::OK);
    std::vector<math::Matrix4> reference, referenceGlobals;
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(bake, second, reference, referenceGlobals), BoneBakeTestStatus::OK);
    for (size_t i = 0; i < matrices.size(); ++i) ExpectBoneBakeTestMatrix(matrices[i], reference[i]);
}

TEST_F(BakedBoneAnimationTest, CodecRoundTripPreservesSamplesAndRejectsCorruptOrHostileLengths)
{
    const auto b = MakeBoneBakeTestAsset(); std::vector<std::byte> bytes;
    ASSERT_EQ(asset::EncodeBoneBake(b, bytes), BoneBakeTestStatus::OK); asset::BakedBoneAnimation decoded;
    ASSERT_EQ(asset::DecodeBoneBake(bytes, decoded), BoneBakeTestStatus::OK);
    EXPECT_EQ(decoded.skeletonFingerprint, b.skeletonFingerprint); EXPECT_EQ(decoded.samples.size(), b.samples.size());
    std::vector<math::Matrix4> p, g, q, h;
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(b, MakeBoneBakeTestFrame(b, 0.8), p, g), BoneBakeTestStatus::OK);
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(decoded, MakeBoneBakeTestFrame(decoded, 0.8), q, h), BoneBakeTestStatus::OK);
    for (size_t i = 0; i < p.size(); ++i) ExpectBoneBakeTestMatrix(p[i], q[i], 0.0f);
    decoded.clipFingerprint = 42;
    for (size_t n : {size_t{0}, size_t{8}, size_t{127}, bytes.size() - 1})
        EXPECT_EQ(asset::DecodeBoneBake(std::span<const std::byte>(bytes).first(n), decoded), BoneBakeTestStatus::INVALID_DATA);
    auto bad = bytes; bad[150] ^= std::byte{1}; EXPECT_EQ(asset::DecodeBoneBake(bad, decoded), BoneBakeTestStatus::INVALID_DATA);
    bad = bytes; for (size_t i = 56; i < 60; ++i) bad[i] = std::byte{255}; RepairBoneBakeTestChecksum(bad);
    EXPECT_EQ(asset::DecodeBoneBake(bad, decoded), BoneBakeTestStatus::INVALID_DATA); EXPECT_EQ(decoded.clipFingerprint, 42u);
}

TEST_F(BakedBoneAnimationTest, SharedPoseIsOptInAndCopiesOutputsAcrossActors)
{
    const auto source = std::make_shared<const asset::BakedBoneAnimation>(MakeBoneBakeTestAsset());
    scene::BonePoseSharing memo; asset::BakedBoneWorkspace workspace;
    std::vector<math::Matrix4> palette,globals,reference,referenceGlobals;
    auto frame = MakeBoneBakeTestFrame(*source,0.50001); asset::BakedBoneFrame targetFrame;
    const auto original = frame; bool hit = false;
    ASSERT_EQ(memo.Evaluate(1,10,0,source,frame,{},targetFrame,0,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_FALSE(hit); EXPECT_EQ(frame.first,original.first); EXPECT_EQ(frame.alpha,original.alpha);
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(*source,original,reference,referenceGlobals),BoneBakeTestStatus::OK);
    for (size_t index=0;index<palette.size();++index) ExpectBoneBakeTestMatrix(palette[index],reference[index]);
    frame = MakeBoneBakeTestFrame(*source,0.50001);
    ASSERT_EQ(memo.Evaluate(2,10,120,source,frame,{},targetFrame,0,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_FALSE(hit); const auto expected = palette;
    palette[0].m[0][0] = -999;
    frame = MakeBoneBakeTestFrame(*source,0.50002);
    ASSERT_EQ(memo.Evaluate(2,10,120,source,frame,{},targetFrame,0,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_TRUE(hit);
    for (size_t index=0;index<palette.size();++index) ExpectBoneBakeTestMatrix(palette[index],expected[index]);
    ASSERT_EQ(memo.Evaluate(2,11,120,source,frame,{},targetFrame,0,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_FALSE(hit);
    ASSERT_EQ(memo.Evaluate(3,11,120,source,frame,{},targetFrame,0,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_FALSE(hit);
}

TEST_F(BakedBoneAnimationTest, SharedPoseKeysIncludeTargetClipAndBlendWeight)
{
    auto clip = MakeBoneBakeTestSwim(); clip.tracks[1].rotations[1].value = math::Quaternion::FromAxisAngle(math::Vector3::UP,2.4f);
    const auto source = std::make_shared<const asset::BakedBoneAnimation>(MakeBoneBakeTestAsset());
    const auto target = std::make_shared<const asset::BakedBoneAnimation>(MakeBoneBakeTestAsset(MakeBoneBakeTestRig(),clip));
    scene::BonePoseSharing memo; asset::BakedBoneWorkspace workspace;
    std::vector<math::Matrix4> palette,globals; bool hit=false;
    auto frame=MakeBoneBakeTestFrame(*source,0.5),other=MakeBoneBakeTestFrame(*target,0.5);
    ASSERT_EQ(memo.Evaluate(1,10,120,source,frame,target,other,0.25f,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_FALSE(hit); const float first=palette[1].m[0][0];
    ASSERT_EQ(memo.Evaluate(1,10,120,source,frame,target,other,0.75f,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_FALSE(hit); EXPECT_GT(std::abs(palette[1].m[0][0]-first),0.0001f);
    ASSERT_EQ(memo.Evaluate(1,10,120,source,frame,source,other,0.75f,palette,globals,workspace,hit),BoneBakeTestStatus::OK);
    EXPECT_FALSE(hit);
}

TEST_F(BakedBoneAnimationTest, SharedPoseQuantizationBoundsReversePauseAndSeamTimeError)
{
    const auto source=MakeBoneBakeTestAsset();
    for(uint32_t index=0;index<1000;++index) {
        const double speed=index%3==0?-0.5:index%3==1?0:1.25;
        asset::BakedBoneFrame frame;
        ASSERT_EQ(asset::ResolveBakedBoneFrame(source,1.15,speed,index*0.007,true,frame),BoneBakeTestStatus::OK);
        const double before=(frame.first+static_cast<double>(frame.alpha))*source.durationSeconds/(source.frameCount-1);
        ASSERT_EQ(scene::BonePoseSharing::Quantize(source,120,frame),BoneBakeTestStatus::OK);
        const double after=(frame.first+static_cast<double>(frame.alpha))*source.durationSeconds/(source.frameCount-1);
        EXPECT_LE(std::abs(after-before),1.0/240.0+0.0000002);
    }
    auto before=MakeBoneBakeTestFrame(source,1.9999),after=MakeBoneBakeTestFrame(source,0.0001);
    ASSERT_EQ(scene::BonePoseSharing::Quantize(source,120,before),BoneBakeTestStatus::OK);
    ASSERT_EQ(scene::BonePoseSharing::Quantize(source,120,after),BoneBakeTestStatus::OK);
    std::vector<math::Matrix4> a,b,g;
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(source,before,a,g),BoneBakeTestStatus::OK);
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(source,after,b,g),BoneBakeTestStatus::OK);
    for(size_t index=0;index<a.size();++index) ExpectBoneBakeTestMatrix(a[index],b[index]);
    before.alpha=std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(scene::BonePoseSharing::Quantize(source,120,before),BoneBakeTestStatus::INVALID_DATA);
}

namespace {
struct BoneBatchTestWrite { size_t buffer=0; std::vector<uint8_t> bytes; };
class BoneBatchTestBuffer final : public renderer::IConstantBuffer {
public:
    BoneBatchTestBuffer(size_t size,size_t index,std::vector<BoneBatchTestWrite>& writes):m_size(size),m_index(index),m_writes(writes){}
    void Update(const void* data,size_t size) override {
        BoneBatchTestWrite record;record.buffer=m_index;record.bytes.resize(size);
        std::memcpy(record.bytes.data(),data,size);m_writes.push_back(std::move(record));
    }
    size_t GetSize() const override {return m_size;}
private:
    size_t m_size=0,m_index=0;std::vector<BoneBatchTestWrite>& m_writes;
};
class BoneBatchTestRenderer final : public BoneBakeTestCpuRenderer {
public:
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override {
        return std::make_unique<BoneBatchTestBuffer>(size,nextBuffer++,writes);
    }
    bool IsRenderThread() const override {return rendering;}
    /// @note Synchronous resource operations establish render-thread identity, including ResourceManager teardown.
    void ExecuteResourceOperation(const std::function<void()>& operation) override {
        const bool previous=rendering;rendering=true;operation();rendering=previous;
    }
    bool RecordResourceUpdate(std::function<void()> operation) override {operations.push_back(std::move(operation));return true;}
    void Drain(){rendering=true;for(auto& operation:operations)operation();operations.clear();rendering=false;}
    std::vector<BoneBatchTestWrite> writes;std::vector<std::function<void()>> operations;
    size_t nextBuffer=0;bool rendering=false;
};
}
TEST_F(BakedBoneAnimationTest, BatchedCompatibilityBytesKeepFullIdentityTailAndDeferredOwnership)
{
    BoneBatchTestRenderer renderer;renderer::ResourceManager resources(renderer);
    const auto first=resources.CreateConstantBuffer(8192);
    const auto second=resources.CreateConstantBuffer(8192);
    ASSERT_TRUE(first.IsValid());ASSERT_TRUE(second.IsValid());
    auto batch=std::make_shared<renderer::ConstantBufferUpdateBatch>();batch->bytes.resize(16384);
    std::array<math::Matrix4,128> identity{};for(auto& matrix:identity)matrix=math::Matrix4::Identity();
    std::memcpy(batch->bytes.data(),identity.data(),8192);std::memcpy(batch->bytes.data()+8192,identity.data(),8192);
    const auto expected=batch->bytes;
    batch->entries={{first,0,8192},{second,8192,8192}};
    resources.UpdateConstantBuffers(batch);batch.reset();
    EXPECT_TRUE(renderer.writes.empty());ASSERT_EQ(renderer.operations.size(),1u);
    renderer.Drain();ASSERT_EQ(renderer.writes.size(),2u);
    EXPECT_EQ(renderer.writes[0].buffer,0u);EXPECT_EQ(renderer.writes[1].buffer,1u);
    EXPECT_EQ(renderer.writes[0].bytes,std::vector<uint8_t>(expected.begin(),expected.begin()+8192));
    EXPECT_EQ(renderer.writes[1].bytes,std::vector<uint8_t>(expected.begin()+8192,expected.end()));
}

TEST_F(BakedBoneAnimationTest, InvalidBatchByteRangePublishesNoPartialUpdate)
{
    BoneBatchTestRenderer renderer;renderer::ResourceManager resources(renderer);
    const auto handle=resources.CreateConstantBuffer(8192);ASSERT_TRUE(handle.IsValid());
    auto batch=std::make_shared<renderer::ConstantBufferUpdateBatch>();batch->bytes.resize(8192);
    batch->entries={{handle,0,8192},{handle,8191,8}};
    resources.UpdateConstantBuffers(batch);renderer.Drain();EXPECT_TRUE(renderer.writes.empty());
}

TEST_F(BakedBoneAnimationTest, CpuBoneFallbackFlushesAnOwnedPacketBeforeIndividualUpdates)
{
    BoneBatchTestRenderer renderer;renderer::ResourceManager resources(renderer);
    const auto first=resources.CreateConstantBuffer(8192);
    const auto second=resources.CreateConstantBuffer(8192);
    std::vector<math::Matrix4> palette{math::Matrix4::Identity()};
    {
        scene::BoneCbBatch batch(resources,true,2);batch.SetActorEligible(true);
        scene::BoneCbBatch::UpdatePalette(resources,first,palette);
        EXPECT_TRUE(renderer.operations.empty());
        scene::BoneCbBatch::BeforeBoneHierarchy(resources);
        ASSERT_EQ(renderer.operations.size(),1u);
        scene::BoneCbBatch::UpdatePalette(resources,second,palette);
        ASSERT_EQ(renderer.operations.size(),2u);
    }
    renderer.Drain();ASSERT_EQ(renderer.writes.size(),2u);
    EXPECT_EQ(renderer.writes[0].buffer,0u);EXPECT_EQ(renderer.writes[1].buffer,1u);
    for(const auto& write:renderer.writes) EXPECT_EQ(write.bytes.size(),8192u);
}

TEST_F(BakedBoneAnimationTest, ImmediateCompatibilityTemplatePreservesActualPaletteAndIdentityTail)
{
    BoneBatchTestRenderer renderer;renderer.rendering=true;renderer::ResourceManager resources(renderer);
    const auto handle=resources.CreateConstantBuffer(8192);
    const std::vector<math::Matrix4> palette{math::Matrix4::TRS({2,3,4},math::Quaternion::Identity(),{2,1,0.5f})};
    scene::BoneCbBatch::UpdatePalette(resources,handle,palette);
    ASSERT_EQ(renderer.writes.size(),1u);ASSERT_EQ(renderer.writes[0].bytes.size(),8192u);
    EXPECT_EQ(std::memcmp(renderer.writes[0].bytes.data(),palette.data(),sizeof(math::Matrix4)),0);
    const auto identity=math::Matrix4::Identity();
    for(size_t bone=1;bone<128;++bone)
        EXPECT_EQ(std::memcmp(renderer.writes[0].bytes.data()+bone*sizeof(math::Matrix4),&identity,sizeof(identity)),0);
    EXPECT_TRUE(renderer.operations.empty());
}

} /// @note namespace fbzz::tests
