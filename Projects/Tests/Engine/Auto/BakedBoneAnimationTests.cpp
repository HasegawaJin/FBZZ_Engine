/// @file    BakedBoneAnimationTests.cpp
/// @brief   Bone bake clocks, quaternion interpolation, bounds, cache and corruption contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Scene/Systems/IKSystem.hpp>
#include <Engine/Scene/Systems/SpringBoneSystem.hpp>
#include <Engine/Scene/Systems/RagdollSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Core/ILogSink.hpp>
#include <Math/MathUtils.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Scheduler/SystemScheduler.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Core/Time.hpp>
#include <Physics/World.hpp>
#include "../../../Engine/src/Scene/Systems/BonePoseSharing.hpp"
#include "../../../Engine/src/Scene/Systems/BoneCbBatch.hpp"
#include <cstring>
#include <functional>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/BakedBoneAnimation.hpp>
#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Format/FzAssetFormat.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/Util/FileSystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/BoneBakeRuntime.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <sstream>

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

asset::Model MakeBoneBakeRuntimeTestModel()
{
    asset::Model model;
    model.skeleton = std::make_unique<asset::Skeleton>(MakeBoneBakeTestRig());
    auto mesh = std::make_unique<renderer::Mesh>();
    renderer::SkinnedVertex vertex{};
    vertex.position = {3, 0, 0}; vertex.boneWeights[0] = 1;
    mesh->cpuSkinnedVertices.push_back(vertex);
    model.meshes.push_back(std::move(mesh));
    return model;
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

/// @note FZAN v3 input fixture follows AnimationImporter; the production BAT codec is not used to manufacture its source.
bool WriteBoneBakeTestClip(const std::string& path, const asset::AnimationClip& clip)
{
    struct Extension {
        double duration; float rate; uint8_t loop, root, xz, y, rotation, optimized, padding[2];
        uint32_t rootTrack, events, properties; float positionError, rotationError, scaleError;
    };
    struct Track {
        char path[256], node[128]; uint8_t interpolation, padding[3]; uint32_t positions, rotations, scales;
    };
    struct Event { double time; char name[64]; int32_t integer; float scalar; };
    static_assert(sizeof(Extension)==48 && sizeof(Track)==400 && sizeof(Event)==80);
    std::vector<std::byte> bytes;
    const auto append = [&bytes](const auto& value) {
        const auto view = std::as_bytes(std::span(&value,1)); bytes.insert(bytes.end(),view.begin(),view.end());
    };
    asset::FzAnimHeader header{};
    std::copy_n("FZAN",4,header.magic); header.version=3;
    std::copy(clip.name.begin(),clip.name.end(),header.name);
    header.durationTicks=clip.durationTicks; header.ticksPerSecond=clip.ticksPerSecond;
    header.trackCount=static_cast<uint32_t>(clip.tracks.size()); append(header);
    Extension extension{}; extension.duration=clip.durationSeconds; extension.rate=60; extension.loop=clip.loop;
    extension.root=clip.hasRootMotion;extension.rootTrack=clip.rootMotionTrackIndex;extension.xz=clip.rootMotionApplyXZ;
    extension.y=clip.rootMotionApplyY;extension.rotation=clip.rootMotionApplyRotation;
    extension.events=static_cast<uint32_t>(clip.events.size());append(extension);
    for (const auto& source : clip.events) {
        Event event{};event.time=source.time;event.integer=source.intParam;event.scalar=source.floatParam;
        std::copy_n(source.name.begin(),(std::min)(source.name.size(),sizeof(event.name)-1),event.name);append(event);
    }
    for (const auto& source : clip.tracks) {
        Track track{}; std::copy(source.nodeName.begin(),source.nodeName.end(),track.node);
        track.interpolation=static_cast<uint8_t>(source.interp);
        track.positions=static_cast<uint32_t>(source.positions.size()); track.rotations=static_cast<uint32_t>(source.rotations.size());
        track.scales=static_cast<uint32_t>(source.scales.size()); append(track);
        for (const auto& key : source.positions) append(asset::FzVectorKey{key.time,key.value.x,key.value.y,key.value.z,0});
        for (const auto& key : source.rotations) append(asset::FzQuaternionKey{key.time,key.value.x,key.value.y,key.value.z,key.value.w});
        for (const auto& key : source.scales) append(asset::FzVectorKey{key.time,key.value.x,key.value.y,key.value.z,0});
    }
    std::string payload(bytes.size(),'\0');
    for (size_t i=0;i<bytes.size();++i) payload[i]=std::bit_cast<char>(bytes[i]);
    return util::FileSystem::WriteTextAtomic(path,payload);
}

struct BoneBakeRuntimeTestAssets {
    testkit::TempDir temp{"BoneBakeRuntime"};
    BoneBakeTestCpuRenderer renderer;
    renderer::ResourceManager resources{renderer};
    BoneBakeRuntimeTestAssets() { asset::AssetManager::Init(resources,temp.File("Assets").generic_string()+"/"); }
    ~BoneBakeRuntimeTestAssets() { asset::AssetManager::UnloadAll(); }
};
/// @note Value-owned mutable clips exercise the supported Animator input contract without baking or asset import.
scene::GameObject& AddBoneBakeTestLegacyActor(scene::Scene& scene, asset::Model& model,
                                             asset::AnimationClip clip, const char* name)
{
    auto& actor = scene.CreateGameObject(name);
    actor.AddComponent<scene::SkinnedMeshRenderer>().model = &model;
    auto& animator = actor.AddComponent<scene::AnimatorComponent>();
    animator.clips.push_back(std::move(clip));
    animator.clipsLoaded = true;
    animator.appliedAssetGeneration = asset::AssetManager::GetAssetGeneration();
    animator.boneBakeMode = static_cast<int>(scene::BoneBakeMode::LEGACY);
    animator.rootMotion.mode = scene::RootMotionMode::None;
    scene::AnimationState state;
    state.name = "Swim";
    state.clipName = animator.clips.front().name;
    state.clipIndex = 0;
    animator.states.push_back(state);
    animator.defaultStateName = state.name;
    return actor;
}

class BoneBakeTestPoseCallback : public scene::Script {
public:
    std::function<void()> onMove;
    std::function<void(const scene::AnimationEventInfo&)> onEvent;
    void OnAnimatorMove(const scene::RootMotionInfo&) override { if (onMove) onMove(); }
    void OnAnimationEvent(const scene::AnimationEventInfo& event) override { if (onEvent) onEvent(event); }
};

/// @note Real mask-load logging may call native sinks between legacy FK and Bone application.
class BoneBakeTestMaskReloadSink : public core::ILogSink {
public:
    explicit BoneBakeTestMaskReloadSink(std::function<void()> action) : m_action(std::move(action))
    {
        core::Logger::AddSink(this);
    }
    ~BoneBakeTestMaskReloadSink() override { core::Logger::RemoveSink(this); }
    void OnLog(const core::LogEntry& entry) override
    {
        if (entry.message.find("AnimatorSystem: avatar mask load failed") != std::string::npos) {
            ++hits;
            m_action();
        }
    }
    int hits = 0;
private:
    std::function<void()> m_action;
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

TEST_F(BakedBoneAnimationTest, RuntimeSharesContentAndReimportGenerationReplacesCachedPose)
{
    BoneBakeRuntimeTestAssets assets;
    const auto path = assets.temp.File("Assets/MakeBoneBakeTestSwim.anim").generic_string();
    ASSERT_TRUE(WriteBoneBakeTestClip(path,MakeBoneBakeTestSwim()));
    const auto handle = asset::AssetManager::Load<asset::AnimationClip>(path);
    ASSERT_NE(asset::AssetManager::Get(handle),nullptr);
    auto model = MakeBoneBakeRuntimeTestModel(), otherModel = MakeBoneBakeRuntimeTestModel();
    scene::AnimatorComponent first, second;
    for (auto* animator : {&first, &second}) {
        animator->boneBakeMode = static_cast<int>(scene::BoneBakeMode::CPU_BAKED);
        animator->boneBakeRequireCpuBones = false;
        animator->sharedClips.push_back({handle, {}, 0});
    }
    const auto evaluate = [](scene::AnimatorComponent& animator, const asset::Model& source) {
        const std::array inputs{scene::BoneBakeClipInput{&animator.ClipAt(0), 1.0, 1.0f}};
        return scene::TryEvaluateBoneBake(animator, source, inputs);
    };
    ASSERT_TRUE(evaluate(first, model)); ASSERT_TRUE(evaluate(second, otherModel));
    ASSERT_NE(first.boneBake.source, nullptr);
    EXPECT_EQ(first.boneBake.source, second.boneBake.source);
    const auto oldBake = first.boneBake.source;
    const auto oldPalette = first.boneMatrices;
    auto changed = MakeBoneBakeTestSwim(); changed.tracks[0].positions[1].value.y += 0.5f;
    ASSERT_TRUE(WriteBoneBakeTestClip(path,changed));
    ASSERT_EQ(asset::AssetManager::ReloadPath(path),1);
    first.prevBoneMatricesValid = true;
    ASSERT_TRUE(evaluate(first, model)); ASSERT_TRUE(evaluate(second, otherModel));
    EXPECT_EQ(first.boneBake.source, second.boneBake.source);
    EXPECT_NE(first.boneBake.source, oldBake);
    EXPECT_NE(first.boneBake.source->clipFingerprint, oldBake->clipFingerprint);
    EXPECT_FALSE(first.prevBoneMatricesValid);
    EXPECT_NE(first.boneMatrices[0].m[1][3], oldPalette[0].m[1][3]);
    first.clips = {MakeBoneBakeTestSwim()}; first.sharedClips.clear();
    const auto retained = first.boneMatrices;
    EXPECT_FALSE(evaluate(first, model)); EXPECT_FALSE(first.boneBake.active);
    EXPECT_EQ(first.boneBake.fallback, scene::BoneBakeFallback::MUTABLE_CLIP);
    for (size_t i = 0; i < retained.size(); ++i) ExpectBoneBakeTestMatrix(first.boneMatrices[i], retained[i], 0);
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

TEST_F(BakedBoneAnimationTest, SettingsSurviveSaveAndPassiveCloneAndLegacyScenesStayLegacy)
{
    scene::Scene source;
    auto& animator = source.CreateGameObject("Fish").AddComponent<scene::AnimatorComponent>();
    animator.boneBakeMode = static_cast<int>(scene::BoneBakeMode::GPU_SHARED);
    animator.boneBakeSampleRate = 120; animator.boneBakeMinDistance = 30; animator.boneBakeRequireCpuBones = false;
    animator.boneBake.active = true; animator.boneBake.workspace.globals.resize(3);
    std::string diagnostic;
    const auto text = scene::SceneSerializer::CapturePassiveClone(source, diagnostic);
    ASSERT_FALSE(text.empty()); ASSERT_TRUE(diagnostic.empty());
    const auto check = [](scene::Scene& restored) {
        const auto* value = restored.Find("Fish")->GetComponent<scene::AnimatorComponent>();
        ASSERT_NE(value, nullptr);
        EXPECT_EQ(value->boneBakeMode, static_cast<int>(scene::BoneBakeMode::GPU_SHARED));
        EXPECT_FLOAT_EQ(value->boneBakeSampleRate,120); EXPECT_FLOAT_EQ(value->boneBakeMinDistance,30);
        EXPECT_FALSE(value->boneBakeRequireCpuBones); EXPECT_FALSE(value->boneBake.active);
        EXPECT_TRUE(value->boneBake.workspace.globals.empty());
    };
    auto cloned = scene::SceneSerializer::LoadDataFromText(text, "bone-bake.scene");
    ASSERT_NE(cloned, nullptr); check(*cloned);
    const auto savedText = scene::SceneSerializer::SaveToText(source);
    auto restored = scene::SceneSerializer::LoadDataFromText(savedText, "bone-bake.scene");
    ASSERT_NE(restored, nullptr); check(*restored);
    std::istringstream lines(text); std::string legacy, line;
    while (std::getline(lines,line)) if (line.find("boneBake") == std::string::npos) legacy += line + '\n';
    auto old = scene::SceneSerializer::LoadDataFromText(legacy, "bone-bake.scene");
    ASSERT_NE(old, nullptr);
    const auto* value = old->Find("Fish")->GetComponent<scene::AnimatorComponent>();
    ASSERT_NE(value, nullptr); EXPECT_EQ(value->boneBakeMode, static_cast<int>(scene::BoneBakeMode::LEGACY));
    EXPECT_TRUE(value->boneBakeRequireCpuBones);
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
/// @file    BonePoseSharingTests.inc
/// @brief   Internal shared-pose precision, isolation and owned deferred-CB transfer contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-12
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
    bool RecordResourceUpdate(std::function<void()> operation) override {
        auto hook = std::move(beforeRecord); beforeRecord = {}; if (hook) hook();
        operations.push_back(std::move(operation)); return true;
    }
    void Drain(){rendering=true;for(auto& operation:operations)operation();operations.clear();rendering=false;}
    std::vector<BoneBatchTestWrite> writes;std::vector<std::function<void()>> operations;
    size_t nextBuffer=0;bool rendering=false;
    std::function<void()> beforeRecord;
};

/// @note Each update drains recorded CPU buffer writes; no device, wall clock or renderer view is simulated by this fixture.
struct BoneBakeLodTestWorld {
    testkit::TempDir temp{"BoneBakeLod"};
    BoneBatchTestRenderer renderer;
    renderer::ResourceManager resources{renderer};
    asset::Model model = MakeBoneBakeRuntimeTestModel();
    scene::Scene scene;
    physics::World physics;
    scene::AnimatorSystem system;
    scene::GameObject* actor = nullptr;
    scene::GameObject* camera = nullptr;
    uint64_t savedFrame = Time::frameCount;
    std::string path;

    explicit BoneBakeLodTestWorld(scene::BoneBakeMode mode, const asset::AnimationClip& clip = MakeBoneBakeTestSwim())
    {
        asset::AssetManager::Init(resources,temp.File("Assets").generic_string()+"/");
        path = temp.File("Assets/swim.anim").generic_string();
        EXPECT_TRUE(WriteBoneBakeTestClip(path,clip));
        camera = &scene.CreateGameObject("Camera");
        camera->AddComponent<scene::CameraComponent>();
        actor = &scene.CreateGameObject("Fish");
        actor->transform.position = {100,0,0};
        actor->AddComponent<scene::SkinnedMeshRenderer>().model = &model;
        auto& animator = actor->AddComponent<scene::AnimatorComponent>();
        scene::AnimationState state;state.name="Swim";state.sourcePath=path;state.clipName=clip.name;
        animator.states.push_back(state);animator.defaultStateName=state.name;
        animator.rootMotion.mode=scene::RootMotionMode::None;
        animator.boneBakeMode=static_cast<int>(mode);animator.boneBakeRequireCpuBones=false;
        animator.boneBakeMinDistance=10;
        EXPECT_TRUE(animator.SetUpdateLod(true,1,2,0.125f,0.25f));
        scene::FlushWorldTransforms(scene);
    }
    ~BoneBakeLodTestWorld()
    {
        renderer.Drain();asset::AssetManager::UnloadAll();Time::frameCount=savedFrame;
    }
    scene::AnimatorComponent& Animator() { return *actor->GetComponent<scene::AnimatorComponent>(); }
    scene::SkinnedMeshRenderer& SkinnedRenderer() { return *actor->GetComponent<scene::SkinnedMeshRenderer>(); }
    void Step(float dt=0.0625f)
    {
        resources.AdvanceFrame();++Time::frameCount;
        SystemContext context{scene,physics,&resources,nullptr,dt,dt,true,true};
        system.Update(context);renderer.Drain();
    }
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


/// @file    HierarchyOnlyTests.inc
/// @brief   Exercise the real Animator update and CPU-reader fallback through imported clip inputs.
/// @author  Hasegawa Jin
/// @date    2026-10-12
TEST_F(BakedBoneAnimationTest, PureBakedAnimatorAvoidsBoneEntitiesAndCpuReaderRestoresThem)
{
    BoneBakeRuntimeTestAssets assets;const auto path=assets.temp.File("Assets/swim.anim").generic_string();
    ASSERT_TRUE(WriteBoneBakeTestClip(path,MakeBoneBakeTestSwim()));
    auto model=MakeBoneBakeRuntimeTestModel();scene::Scene scene;physics::World world;
    auto& camera=scene.CreateGameObject("Camera");camera.AddComponent<scene::CameraComponent>();
    auto& fish=scene.CreateGameObject("Fish");fish.AddComponent<scene::SkinnedMeshRenderer>().model=&model;
    auto& animator=fish.AddComponent<scene::AnimatorComponent>();
    scene::AnimationState state;state.name="Swim";state.sourcePath=path;state.clipName="MakeBoneBakeTestSwim";
    animator.states.push_back(state);animator.defaultStateName="Swim";animator.rootMotion.mode=scene::RootMotionMode::None;
    animator.boneBakeMode=static_cast<int>(scene::BoneBakeMode::CPU_BAKED);animator.boneBakeRequireCpuBones=false;
    animator.boneBakeMinDistance=0;scene::AnimatorSystem system;
    SystemContext context{scene,world,&assets.resources,nullptr,1.0f/60,1.0f/60,true,true};
    system.Update(context);
    ASSERT_TRUE(animator.boneBake.active);EXPECT_EQ(animator.boneBake.fallback,scene::BoneBakeFallback::NONE);
    EXPECT_TRUE(scene.GetEntities<scene::BoneComponent>().empty());EXPECT_EQ(animator.boneMatrices.size(),2u);
    EXPECT_EQ(animator.nodeGlobalTransforms.size(),3u);
    animator.boneBakeRequireCpuBones=true;system.Update(context);
    EXPECT_FALSE(animator.boneBake.active);EXPECT_EQ(animator.boneBake.fallback,scene::BoneBakeFallback::CPU_BONES_REQUIRED);
    EXPECT_EQ(scene.GetEntities<scene::BoneComponent>().size(),3u);
}
TEST_F(BakedBoneAnimationTest, NearBakedAnimatorKeepsOrdinaryBoneHierarchy)
{
    BoneBakeRuntimeTestAssets assets;const auto path=assets.temp.File("Assets/swim.anim").generic_string();
    ASSERT_TRUE(WriteBoneBakeTestClip(path,MakeBoneBakeTestSwim()));
    auto model=MakeBoneBakeRuntimeTestModel();scene::Scene scene;physics::World world;
    auto& camera=scene.CreateGameObject("Camera");camera.AddComponent<scene::CameraComponent>();
    auto& fish=scene.CreateGameObject("Fish");fish.AddComponent<scene::SkinnedMeshRenderer>().model=&model;
    auto& animator=fish.AddComponent<scene::AnimatorComponent>();
    scene::AnimationState state;state.name="Swim";state.sourcePath=path;state.clipName="MakeBoneBakeTestSwim";
    animator.states.push_back(state);animator.defaultStateName="Swim";animator.rootMotion.mode=scene::RootMotionMode::None;
    animator.boneBakeMode=static_cast<int>(scene::BoneBakeMode::CPU_BAKED);animator.boneBakeRequireCpuBones=false;
    animator.boneBakeMinDistance=10;scene::AnimatorSystem system;
    SystemContext context{scene,world,&assets.resources,nullptr,1.0f/60,1.0f/60,true,true};system.Update(context);
    EXPECT_FALSE(animator.boneBake.active);EXPECT_EQ(animator.boneBake.fallback,scene::BoneBakeFallback::NEAR_CAMERA);
    EXPECT_EQ(scene.GetEntities<scene::BoneComponent>().size(),3u);
}

TEST_F(BakedBoneAnimationTest, RagdollSamplesFreshAnimationAcrossDistanceLodAndBakeModes)
{
    for (const auto mode : {scene::BoneBakeMode::LEGACY, scene::BoneBakeMode::CPU_BAKED,
                           scene::BoneBakeMode::GPU_SHARED, scene::BoneBakeMode::GPU_POSE_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));
        BoneBakeLodTestWorld world(mode);
        auto& ragdoll = world.actor->AddComponent<scene::RagdollComponent>();
        ragdoll.rootBoneName = "Body";
        ragdoll.activateOnStart = true;
        ragdoll.blendIn = 0.0f;
        ragdoll.groundPlane = false;
        ragdoll.contactWorld = ragdoll.contactDynamic = ragdoll.contactSelf = false;
        scene::RagdollSystem ragdollSystem;

        for (int frame = 1; frame <= 4; ++frame) {
            world.Step();
            auto& animator = world.Animator();
            EXPECT_TRUE(animator.poseEvaluatedThisFrame);
            EXPECT_FALSE(animator.boneBake.active);
            if (mode != scene::BoneBakeMode::LEGACY)
                EXPECT_EQ(animator.boneBake.fallback, scene::BoneBakeFallback::RIG_CONTROL);
            ASSERT_EQ(world.SkinnedRenderer().nodeEntities.size(), 3u);
            if (frame == 1) ragdoll.pendingImpulses.push_back({{}, {0, 2, 0}, 0});
            SystemContext context{world.scene, world.physics, &world.resources, nullptr,
                                  0.0625f, 0.0625f, true, true};
            ragdollSystem.Update(context);
            world.renderer.Drain();

            ASSERT_EQ(ragdoll.runtimeStatus, scene::RagdollStatus::Running);
            ASSERT_FALSE(ragdoll.runtime.bones.empty());
            EXPECT_NEAR(ragdoll.runtime.bones.front().position.y,
                        static_cast<float>(frame) * 0.0625f * 0.2f, 0.0001f);
        }
    }
}

TEST_F(BakedBoneAnimationTest, AddingRagdollToHeldBakeRestoresItsCurrentCpuTargets)
{
    for (const auto mode : {scene::BoneBakeMode::CPU_BAKED, scene::BoneBakeMode::GPU_SHARED,
                           scene::BoneBakeMode::GPU_POSE_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));
        BoneBakeLodTestWorld world(mode);
        world.Step();
        world.Step();
        ASSERT_TRUE(world.Animator().boneBake.active);
        ASSERT_FALSE(world.Animator().poseEvaluatedThisFrame);
        ASSERT_TRUE(world.SkinnedRenderer().nodeEntities.empty());
        auto& ragdoll = world.actor->AddComponent<scene::RagdollComponent>();
        ragdoll.rootBoneName = "Body";
        ragdoll.activateOnStart = true;
        ragdoll.groundPlane = false;
        ragdoll.contactWorld = ragdoll.contactDynamic = ragdoll.contactSelf = false;

        world.Step();
        scene::RagdollSystem ragdollSystem;
        SystemContext context{world.scene, world.physics, &world.resources, nullptr,
                              0.0625f, 0.0625f, true, true};
        ragdollSystem.Update(context);
        world.renderer.Drain();

        EXPECT_FALSE(world.Animator().boneBake.active);
        EXPECT_EQ(world.Animator().boneBake.fallback, scene::BoneBakeFallback::RIG_CONTROL);
        ASSERT_EQ(ragdoll.runtimeStatus, scene::RagdollStatus::Running);
        ASSERT_FALSE(ragdoll.runtime.bones.empty());
        EXPECT_NEAR(ragdoll.runtime.bones.front().position.y, 3.0f * 0.0625f * 0.2f, 0.0001f);
    }
}

TEST_F(BakedBoneAnimationTest, PausePreservesTheFinalIkSpringAndRagdollPoseUntilResume)
{
    BoneBakeLodTestWorld world(scene::BoneBakeMode::LEGACY);
    auto& target = world.scene.CreateGameObject("AimTarget");
    target.transform.position = {103, 2, 1};
    auto& ik = world.actor->AddComponent<scene::IKSolverComponent>();
    scene::IKChain aim;
    aim.type = scene::IKSolverType::AimAt;
    aim.boneNames = {"Tail"};
    aim.targetEntity = target.GetID();
    aim.lookAtAxis = math::Vector3::RIGHT;
    ik.chains.push_back(aim);
    auto& spring = world.actor->AddComponent<scene::SpringBoneComponent>();
    scene::SpringBoneChain tail;
    tail.rootBoneName = "Tail";
    tail.leafTailLength = 0.3f;
    tail.gravityPower = 2.0f;
    tail.weight = 0.3f;
    spring.chains.push_back(tail);
    auto& ragdoll = world.actor->AddComponent<scene::RagdollComponent>();
    ragdoll.rootBoneName = "Body";
    ragdoll.beginRequested = ragdoll.activeRequested = true;
    ragdoll.blendIn = 0.0f;
    ragdoll.activationWeight = 0.5f;
    ragdoll.standingGuard = true;
    ragdoll.groundPlane = false;
    ragdoll.contactWorld = ragdoll.contactDynamic = ragdoll.contactSelf = false;
    SystemScheduler scheduler;
    scheduler.AddSystem<scene::TransformLateUpdate>();
    scheduler.AddSystem<scene::AnimatorSystem>();
    scheduler.AddSystem<scene::IKSystem>();
    scheduler.AddSystem<scene::SpringBoneSystem>();
    scheduler.AddSystem<scene::RagdollSystem>();
    ASSERT_TRUE(scheduler.Build());
    const auto tick = [&](float dt, bool simulating) {
        world.resources.AdvanceFrame();
        ++Time::frameCount;
        SystemContext context{world.scene, world.physics, &world.resources, nullptr,
                              dt, dt, simulating, true};
        scheduler.LateUpdate(context);
        world.renderer.Drain();
    };
    for (int frame = 0; frame < 6; ++frame) tick(0.0625f, true);
    ASSERT_EQ(ragdoll.runtimeStatus, scene::RagdollStatus::Running);
    ASSERT_GT(ik.runtimeSolvedChainCount, 0);
    ASSERT_GT(spring.runtimeSimulatedBoneCount, 0);
    ASSERT_NE(ragdoll.runtime.rig->GetBody(0), nullptr);
    const auto* body = ragdoll.runtime.rig->GetBody(0);
    const auto bodyPosition = body->GetPosition();
    const auto bodyRotation = body->GetRotation();
    const auto bodyVelocity = body->GetVelocity();
    const auto palette = world.Animator().boneMatrices;
    const auto globals = world.Animator().nodeGlobalTransforms;
    std::vector<math::Matrix4> boneWorld;
    for (const auto entity : world.SkinnedRenderer().nodeEntities)
        boneWorld.push_back(world.scene.GetGameObject(entity)->transform.GetWorldMatrix());
    const float stateTime = world.Animator().stateTime;
    const auto poseCount = world.Animator().poseEvaluationCount;
    const auto ikUpdates = ik.runtimeUpdateCount;
    const auto springUpdates = spring.runtimeUpdateCount;
    target.transform.position = {98, -2, -1};

    for (int frame = 0; frame < 2; ++frame) {
        tick(0.5f, false);

        EXPECT_FLOAT_EQ(world.Animator().stateTime, stateTime);
        EXPECT_EQ(world.Animator().poseEvaluationCount, poseCount);
        EXPECT_EQ(ik.runtimeUpdateCount, ikUpdates);
        EXPECT_EQ(spring.runtimeUpdateCount, springUpdates);
        EXPECT_VEC3_NEAR(body->GetPosition(), bodyPosition, 0.000001f);
        EXPECT_VEC3_NEAR(body->GetVelocity(), bodyVelocity, 0.000001f);
        EXPECT_VEC3_NEAR(body->GetRotation() * math::Vector3::RIGHT,
                         bodyRotation * math::Vector3::RIGHT, 0.000001f);
        EXPECT_VEC3_NEAR(body->GetRotation() * math::Vector3::UP,
                         bodyRotation * math::Vector3::UP, 0.000001f);
        ASSERT_EQ(world.Animator().prevBoneMatrices.size(), palette.size());
        for (size_t index = 0; index < palette.size(); ++index) {
            ExpectBoneBakeTestMatrix(world.Animator().boneMatrices[index], palette[index]);
            ExpectBoneBakeTestMatrix(world.Animator().prevBoneMatrices[index], palette[index]);
        }
        for (size_t index = 0; index < globals.size(); ++index) {
            ExpectBoneBakeTestMatrix(world.Animator().nodeGlobalTransforms[index], globals[index]);
            const auto* bone = world.scene.GetGameObject(world.SkinnedRenderer().nodeEntities[index]);
            ExpectBoneBakeTestMatrix(bone->transform.GetWorldMatrix(), boneWorld[index]);
        }
    }

    tick(0.0625f, true);

    EXPECT_GT(world.Animator().stateTime, stateTime);
    EXPECT_GT(world.Animator().poseEvaluationCount, poseCount);
    EXPECT_GT(ik.runtimeUpdateCount, ikUpdates);
    EXPECT_GT(spring.runtimeUpdateCount, springUpdates);
    SystemContext editContext{world.scene, world.physics, &world.resources, nullptr,
                              0.0625f, 0.0625f, false, false};
    world.system.Update(editContext);
    world.renderer.Drain();
    const auto* editBody = world.scene.GetGameObject(world.SkinnedRenderer().nodeEntities[1]);
    EXPECT_NEAR(editBody->transform.worldPosition.y, 0.0f, 0.000001f);
    scheduler.Shutdown();
}

TEST_F(BakedBoneAnimationTest, PauseKeepsTheFirstResumedRootMotionDeltaWithoutRevivingInvalidHistory)
{
    for (int interruption = 0; interruption < 3; ++interruption) {
        SCOPED_TRACE(interruption);
        auto clip = MakeBoneBakeTestSwim();
        clip.hasRootMotion = true;
        clip.rootMotionTrackIndex = 0;
        clip.rootMotionApplyXZ = true;
        clip.rootMotionApplyY = false;
        clip.rootMotionApplyRotation = false;
        clip.tracks[0].positions = {{0.0, {1, 0, 0}}, {2.0, {3, 0, 0}}};
        BoneBakeLodTestWorld world(scene::BoneBakeMode::LEGACY, clip);
        world.Animator().rootMotion.mode = scene::RootMotionMode::ExtractOnly;
        const auto tick = [&](float dt, bool simulating) {
            world.resources.AdvanceFrame();
            ++Time::frameCount;
            SystemContext context{world.scene, world.physics, &world.resources, nullptr,
                                  dt, dt, simulating, true};
            world.system.Update(context);
            world.renderer.Drain();
        };
        tick(0.25f, true);
        tick(0.25f, true);
        ASSERT_NEAR(world.Animator().rootMotionDeltaPosition.x, 0.25f, 0.000001f);
        const float pausedTime = world.Animator().stateTime;
        if (interruption == 1) ++Time::frameCount;
        if (interruption == 2) asset::AssetManager::BumpAssetGeneration();

        for (int frame = 0; frame < 2; ++frame) {
            tick(0.5f, false);
            EXPECT_FLOAT_EQ(world.Animator().stateTime, pausedTime);
            EXPECT_VEC3_NEAR(world.Animator().rootMotionDeltaPosition, math::Vector3::ZERO, 0.000001f);
        }
        tick(0.25f, true);

        EXPECT_NEAR(world.Animator().rootMotionDeltaPosition.x, interruption == 0 ? 0.25f : 0.0f, 0.000001f);
        EXPECT_FALSE(world.Animator().rootMotionAppliedByEngine);
        tick(0.25f, true);
        EXPECT_NEAR(world.Animator().rootMotionDeltaPosition.x, 0.25f, 0.000001f);
    }
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
TEST_F(BakedBoneAnimationTest, LegacyPoseReuseKeepsWeightedTrsAndDistinctActorsExact)
{
    BoneBakeRuntimeTestAssets assets;
    auto model = MakeBoneBakeRuntimeTestModel();
    scene::Scene scene;
    physics::World world;
    const auto clip = MakeBoneBakeTestSwim();
    auto& first = AddBoneBakeTestLegacyActor(scene, model, clip, "First");
    auto& second = AddBoneBakeTestLegacyActor(scene, model, clip, "Second");
    auto& secondAnimator = *second.GetComponent<scene::AnimatorComponent>();
    auto stronger = clip;
    stronger.name = "Stronger";
    stronger.tracks[0].positions[1].value.y = 0.6f;
    stronger.tracks[0].scales = {{0.0, {3, 3, 3}}};
    stronger.tracks[1].rotations[1].value = math::Quaternion::FromAxisAngle(math::Vector3::UP, 2.0f);
    secondAnimator.clips.push_back(std::move(stronger));
    secondAnimator.speed = 3.0f;
    auto& state = secondAnimator.states.front();
    state.mode = scene::AnimationStateMode::BlendTree1D;
    state.blendTree1D.paramName = "Blend";
    scene::BlendTreeMotion baseMotion;
    baseMotion.clipIndex = 0;
    scene::BlendTreeMotion strongMotion;
    strongMotion.threshold = 1.0f;
    strongMotion.clipIndex = 1;
    state.blendTree1D.motions = {baseMotion, strongMotion};
    secondAnimator.parameters.push_back({"Blend", scene::ParamType::Float, 0.25f});
    second.transform.position = {10, 2, 3};
    second.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.4f);
    second.transform.scale = {2, 2, 2};
    scene::FlushWorldTransforms(scene);
    scene::AnimatorSystem system;
    SystemContext context{scene, world, &assets.resources, nullptr, 0.25f, 0.25f, true, true};
    system.Update(context);
    auto& firstAnimator = *first.GetComponent<scene::AnimatorComponent>();
    const auto firstReference = SampleBoneBakeTestReference(*model.skeleton, clip, 0.25);
    ASSERT_EQ(firstAnimator.boneMatrices.size(), firstReference.size());
    for (size_t i = 0; i < firstReference.size(); ++i)
        ExpectBoneBakeTestMatrix(firstAnimator.boneMatrices[i], firstReference[i]);
    auto expectedBlend = clip;
    expectedBlend.tracks[0].positions = {{0.0, {1, 0.225f, 0}}};
    expectedBlend.tracks[0].scales = {{0.0, {1.5f, 1.5f, 1.5f}}};
    expectedBlend.tracks[1].rotations = {{0.0, math::Quaternion::FromAxisAngle(math::Vector3::UP, 1.05f)}};
    const auto secondReference = SampleBoneBakeTestReference(*model.skeleton, expectedBlend, 0.0);
    ASSERT_EQ(secondAnimator.boneMatrices.size(), secondReference.size());
    for (size_t i = 0; i < secondReference.size(); ++i)
        ExpectBoneBakeTestMatrix(secondAnimator.boneMatrices[i], secondReference[i]);
    const auto& secondSmr = *second.GetComponent<scene::SkinnedMeshRenderer>();
    ASSERT_EQ(secondSmr.nodeEntities.size(), 3u);
    const auto* secondBody = scene.GetGameObject(secondSmr.nodeEntities[1]);
    ASSERT_NE(secondBody, nullptr);
    EXPECT_NEAR(secondBody->transform.position.y, 0.225f, 0.0001f);
    EXPECT_NEAR(secondBody->transform.scale.x, 1.5f, 0.0001f);
    system.Update(context);
    const auto nextReference = SampleBoneBakeTestReference(*model.skeleton, clip, 0.5);
    for (size_t i = 0; i < nextReference.size(); ++i)
        ExpectBoneBakeTestMatrix(firstAnimator.boneMatrices[i], nextReference[i]);
}

TEST_F(BakedBoneAnimationTest, LegacyPoseReuseKeepsFirstExactAndFirstCanonicalTrackAfterReplacement)
{
    BoneBakeRuntimeTestAssets assets;
    auto model = MakeBoneBakeRuntimeTestModel();
    scene::Scene scene;
    physics::World world;
    auto clip = MakeBoneBakeTestSwim();
    const auto track = [](const char* name, float x) {
        asset::NodeAnimationTrack result;
        result.nodeName = name;
        result.positions = {{0.0, {x, 0, 0}}};
        return result;
    };
    clip.tracks = {track("A/Body", 100), track("B:Body", 200),
                   track("Body", 3), track("Body", 9), clip.tracks.back()};
    auto& actor = AddBoneBakeTestLegacyActor(scene, model, clip, "Tracks");
    auto& animator = *actor.GetComponent<scene::AnimatorComponent>();
    auto& smr = *actor.GetComponent<scene::SkinnedMeshRenderer>();
    scene::AnimatorSystem system;
    SystemContext context{scene, world, &assets.resources, nullptr, 0.25f, 0.25f, true, true};
    system.Update(context);
    ASSERT_EQ(smr.nodeEntities.size(), 3u);
    auto* body = scene.GetGameObject(smr.nodeEntities[1]);
    ASSERT_NE(body, nullptr);
    EXPECT_NEAR(body->transform.position.x, 3.0f, 0.0001f);
    animator.clips[0].tracks.erase(animator.clips[0].tracks.begin() + 2, animator.clips[0].tracks.begin() + 4);
    system.Update(context);
    EXPECT_NEAR(body->transform.position.x, 100.0f, 0.0001f);
    std::swap(animator.clips[0].tracks[0], animator.clips[0].tracks[1]);
    system.Update(context);
    EXPECT_NEAR(body->transform.position.x, 200.0f, 0.0001f);
    auto replacement = MakeBoneBakeTestSwim();
    replacement.tracks[0].positions = {{0.0, {7, 0, 0}}};
    animator.clips[0] = std::move(replacement);
    system.Update(context);
    EXPECT_NEAR(body->transform.position.x, 7.0f, 0.0001f);
}

TEST_F(BakedBoneAnimationTest, LegacyPoseReusePreservesFirstFkWhenRootMotionCallbackRemovesABone)
{
    BoneBakeRuntimeTestAssets assets;
    auto model = MakeBoneBakeRuntimeTestModel();
    scene::Scene scene;
    physics::World world;
    auto clip = MakeBoneBakeTestSwim();
    clip.hasRootMotion = true;
    clip.rootMotionTrackIndex = 0;
    auto& actor = AddBoneBakeTestLegacyActor(scene, model, clip, "MissingTail");
    auto& animator = *actor.GetComponent<scene::AnimatorComponent>();
    auto& smr = *actor.GetComponent<scene::SkinnedMeshRenderer>();
    animator.rootMotion.mode = scene::RootMotionMode::ExtractOnly;
    scene::EntityID removed = scene::EntityID::INVALID;
    auto callback = std::make_unique<BoneBakeTestPoseCallback>();
    callback->onMove = [&] {
        if (removed.IsValid()) return;
        ASSERT_EQ(smr.nodeEntities.size(), 3u);
        removed = smr.nodeEntities[2];
        EXPECT_TRUE(scene.DestroyGameObject(removed));
    };
    scene::ScriptEntry entry;
    entry.script = std::move(callback);
    actor.AddComponent<scene::ScriptComponent>().scripts.push_back(std::move(entry));
    scene::AnimatorSystem system;
    SystemContext context{scene, world, &assets.resources, nullptr, 0.25f, 0.25f, true, true};
    system.Update(context);
    ASSERT_TRUE(removed.IsValid());
    EXPECT_EQ(scene.GetGameObject(removed), nullptr);
    const auto firstReference = SampleBoneBakeTestReference(*model.skeleton, clip, 0.25);
    ASSERT_EQ(animator.boneMatrices.size(), firstReference.size());
    for (size_t i = 0; i < firstReference.size(); ++i)
        ExpectBoneBakeTestMatrix(animator.boneMatrices[i], firstReference[i]);
    system.Update(context);
    ASSERT_NE(scene.GetGameObject(smr.nodeEntities[2]), nullptr);
    EXPECT_NE(smr.nodeEntities[2], removed);
    const auto nextReference = SampleBoneBakeTestReference(*model.skeleton, clip, 0.5);
    for (size_t i = 0; i < nextReference.size(); ++i)
        ExpectBoneBakeTestMatrix(animator.boneMatrices[i], nextReference[i]);
}

TEST_F(BakedBoneAnimationTest, LegacyPoseReuseKeepsInsertedParentAndInactiveNonBoneChildWorldTransforms)
{
    BoneBakeRuntimeTestAssets assets;
    auto model = MakeBoneBakeRuntimeTestModel();
    scene::Scene scene;
    physics::World world;
    auto& actor = AddBoneBakeTestLegacyActor(scene, model, MakeBoneBakeTestSwim(), "Hierarchy");
    scene::AnimatorSystem system;
    SystemContext context{scene, world, &assets.resources, nullptr, 0.25f, 0.25f, true, true};
    system.Update(context);
    auto& smr = *actor.GetComponent<scene::SkinnedMeshRenderer>();
    auto& animator = *actor.GetComponent<scene::AnimatorComponent>();
    ASSERT_EQ(smr.nodeEntities.size(), 3u);
    auto* root = scene.GetGameObject(smr.nodeEntities[0]);
    auto* body = scene.GetGameObject(smr.nodeEntities[1]);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(body, nullptr);
    auto& spacer = scene.CreateGameObject("Spacer");
    spacer.SetParent(actor);
    spacer.transform.position = {4, 0, 0};
    spacer.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::PI * 0.5f);
    root->SetParent(spacer);
    auto& attachment = scene.CreateGameObject("InactiveAttachment");
    attachment.SetParent(*body);
    attachment.transform.position = {0, 0, 3};
    attachment.SetActive(false);
    scene::FlushWorldTransforms(actor);
    system.Update(context);
    const auto bodyLocal = math::Matrix4::TRS({1, 0.1f, 0}, math::Quaternion::Identity(), math::Vector3::ONE);
    const auto expectedBody = spacer.transform.GetWorldMatrix() * bodyLocal;
    ExpectBoneBakeTestMatrix(body->transform.GetWorldMatrix(), expectedBody);
    ExpectBoneBakeTestMatrix(animator.nodeGlobalTransforms[1], expectedBody);
    ExpectBoneBakeTestMatrix(animator.boneMatrices[0], expectedBody * model.skeleton->bones[0].offsetMatrix);
    const auto expectedAttachment = expectedBody * math::Matrix4::TRS({0, 0, 3}, math::Quaternion::Identity(), math::Vector3::ONE);
    ExpectBoneBakeTestMatrix(attachment.transform.GetWorldMatrix(), expectedAttachment);
    EXPECT_FALSE(attachment.activeInHierarchy());
    EXPECT_EQ(root->GetParent()->GetID(), spacer.GetID());
}

TEST_F(BakedBoneAnimationTest, LegacyPoseReuseKeepsRootMotionBeforePoseAndEventsAfterPose)
{
    BoneBakeRuntimeTestAssets assets;
    auto model = MakeBoneBakeRuntimeTestModel();
    scene::Scene scene;
    physics::World world;
    auto clip = MakeBoneBakeTestSwim();
    clip.hasRootMotion = true;
    clip.rootMotionTrackIndex = 0;
    clip.events.push_back({0.3, "PoseReady", 17, 0.5f});
    auto& actor = AddBoneBakeTestLegacyActor(scene, model, clip, "Callbacks");
    auto& animator = *actor.GetComponent<scene::AnimatorComponent>();
    auto& smr = *actor.GetComponent<scene::SkinnedMeshRenderer>();
    animator.rootMotion.mode = scene::RootMotionMode::ExtractOnly;
    std::vector<std::string> order;
    int moves = 0;
    math::Vector3 eventPosition;
    auto callback = std::make_unique<BoneBakeTestPoseCallback>();
    callback->onMove = [&] {
        order.push_back("move");
        actor.transform.position = {static_cast<float>(++moves) * 10.0f, 0, 0};
        scene::FlushWorldTransforms(actor);
    };
    callback->onEvent = [&](const scene::AnimationEventInfo& event) {
        order.push_back(event.name);
        EXPECT_EQ(event.intParam, 17);
        EXPECT_NEAR(event.floatParam, 0.5f, 0.0001f);
        auto* body = scene.GetGameObject(smr.nodeEntities[1]);
        ASSERT_NE(body, nullptr);
        eventPosition = body->transform.worldPosition;
        animator.clips[0].tracks[0].positions[1].value.y = 1.2f;
    };
    scene::ScriptEntry entry;
    entry.script = std::move(callback);
    actor.AddComponent<scene::ScriptComponent>().scripts.push_back(std::move(entry));
    scene::AnimatorSystem system;
    SystemContext context{scene, world, &assets.resources, nullptr, 0.25f, 0.25f, true, true};
    system.Update(context);
    system.Update(context);
    EXPECT_NEAR(eventPosition.x, 21.0f, 0.0001f);
    EXPECT_NEAR(eventPosition.y, 0.1f, 0.0001f);
    system.Update(context);
    EXPECT_EQ(order, (std::vector<std::string>{"move", "move", "PoseReady", "move"}));
    const auto* body = scene.GetGameObject(smr.nodeEntities[1]);
    ASSERT_NE(body, nullptr);
    EXPECT_NEAR(body->transform.position.y, 0.9f, 0.0001f);
    EXPECT_FALSE(animator.rootMotionAppliedByEngine);
}

TEST_F(BakedBoneAnimationTest, LegacyPoseReuseResamplesAfterInitialAndReplacementMaskIo)
{
    BoneBakeRuntimeTestAssets assets;
    auto model = MakeBoneBakeRuntimeTestModel();
    scene::Scene scene;
    physics::World world;
    auto& actor = AddBoneBakeTestLegacyActor(scene, model, MakeBoneBakeTestSwim(), "MaskIo");
    auto& animator = *actor.GetComponent<scene::AnimatorComponent>();
    animator.baseLayerMask.path = assets.temp.File("Assets/absent-first.mask").generic_string();
    int reloads = 0;
    BoneBakeTestMaskReloadSink sink([&] {
        animator.clips[0].tracks[0].positions[1].value.y = ++reloads == 1 ? 1.2f : 1.8f;
    });
    scene::AnimatorSystem system;
    SystemContext context{scene, world, &assets.resources, nullptr, 0.5f, 0.5f, true, true};
    system.Update(context);
    EXPECT_EQ(sink.hits, 1);
    EXPECT_TRUE(animator.baseLayerMask.failed);
    const auto& smr = *actor.GetComponent<scene::SkinnedMeshRenderer>();
    ASSERT_EQ(smr.nodeEntities.size(), 3u);
    const auto* body = scene.GetGameObject(smr.nodeEntities[1]);
    ASSERT_NE(body, nullptr);
    EXPECT_NEAR(body->transform.position.y, 0.6f, 0.0001f);
    animator.baseLayerMask.path = assets.temp.File("Assets/absent-replacement.mask").generic_string();
    system.Update(context);
    EXPECT_EQ(sink.hits, 2);
    EXPECT_NEAR(body->transform.position.y, 1.8f, 0.0001f);
    EXPECT_EQ(animator.baseLayerMask.loadedPath, animator.baseLayerMask.path);
}

TEST_F(BakedBoneAnimationTest, LegacyTrackBindingsRebindEqualSizeSkeletonsAndReorderedTracksAcrossActors)
{
    BoneBakeRuntimeTestAssets assets;
    auto firstModel = MakeBoneBakeRuntimeTestModel();
    auto secondModel = MakeBoneBakeRuntimeTestModel();
    std::swap(secondModel.skeleton->nodes[1].name, secondModel.skeleton->nodes[2].name);
    scene::Scene scene;
    physics::World world;
    const auto firstClip = MakeBoneBakeTestSwim();
    auto secondClip = firstClip;
    secondClip.tracks[0].positions[1].value.y = 1.2f;
    auto thirdClip = secondClip;
    std::swap(thirdClip.tracks[0], thirdClip.tracks[1]);
    auto& first = AddBoneBakeTestLegacyActor(scene, firstModel, firstClip, "FirstBindings");
    auto& second = AddBoneBakeTestLegacyActor(scene, secondModel, secondClip, "SecondBindings");
    auto& third = AddBoneBakeTestLegacyActor(scene, secondModel, thirdClip, "ThirdBindings");
    scene::AnimatorSystem system;
    SystemContext context{scene, world, &assets.resources, nullptr, 0.25f, 0.25f, true, true};

    for (int frame = 1; frame <= 2; ++frame) {
        system.Update(context);

        const auto verify = [frame](scene::GameObject& actor, const asset::Model& model,
                                    const asset::AnimationClip& clip) {
            const auto& animator = *actor.GetComponent<scene::AnimatorComponent>();
            const auto reference = SampleBoneBakeTestReference(*model.skeleton, clip, frame * 0.25);
            ASSERT_EQ(animator.boneMatrices.size(), reference.size());
            for (size_t index = 0; index < reference.size(); ++index)
                ExpectBoneBakeTestMatrix(animator.boneMatrices[index], reference[index]);
        };
        verify(first, firstModel, firstClip);
        verify(second, secondModel, secondClip);
        verify(third, secondModel, thirdClip);
    }
}

TEST_F(BakedBoneAnimationTest, LegacyTrackBindingsAndTraversalScratchSurviveNestedMaskIoUpdates)
{
    BoneBakeRuntimeTestAssets assets;
    auto outerModel = MakeBoneBakeRuntimeTestModel();
    auto nestedModel = MakeBoneBakeRuntimeTestModel();
    for (int index = 3; index < 9; ++index) {
        asset::SkeletonNode node;
        node.name = "Helper" + std::to_string(index);
        node.parentIndex = index - 1;
        node.bindTranslation = {0, 1, 0};
        node.localBindTransform = math::Matrix4::TRS(node.bindTranslation, node.bindRotation, node.bindScale);
        nestedModel.skeleton->nodes[static_cast<size_t>(index - 1)].children.push_back(index);
        nestedModel.skeleton->nodes.push_back(std::move(node));
    }
    scene::Scene outerScene;
    scene::Scene nestedScene;
    physics::World world;
    const auto clip = MakeBoneBakeTestSwim();
    auto& outer = AddBoneBakeTestLegacyActor(outerScene, outerModel, clip, "OuterBindings");
    auto& nested = AddBoneBakeTestLegacyActor(nestedScene, nestedModel, clip, "NestedBindings");
    auto& outerAnimator = *outer.GetComponent<scene::AnimatorComponent>();
    outerAnimator.baseLayerMask.path = assets.temp.File("Assets/absent-nested.mask").generic_string();
    scene::AnimatorSystem system;
    SystemContext nestedContext{nestedScene, world, &assets.resources, nullptr, 0.5f, 0.5f, true, true};
    BoneBakeTestMaskReloadSink sink([&] {
        system.Update(nestedContext);
        std::swap(outerAnimator.clips[0].tracks[0], outerAnimator.clips[0].tracks[1]);
        outerAnimator.clips[0].tracks[1].positions[1].value.y = 1.2f;
    });
    SystemContext outerContext{outerScene, world, &assets.resources, nullptr, 0.5f, 0.5f, true, true};

    system.Update(outerContext);

    EXPECT_EQ(sink.hits, 1);
    const auto outerReference = SampleBoneBakeTestReference(*outerModel.skeleton, outerAnimator.clips[0], 0.5);
    ASSERT_EQ(outerAnimator.boneMatrices.size(), outerReference.size());
    for (size_t index = 0; index < outerReference.size(); ++index)
        ExpectBoneBakeTestMatrix(outerAnimator.boneMatrices[index], outerReference[index]);
    const auto& nestedAnimator = *nested.GetComponent<scene::AnimatorComponent>();
    const auto nestedReference = SampleBoneBakeTestReference(*nestedModel.skeleton, clip, 0.5);
    ASSERT_EQ(nestedAnimator.boneMatrices.size(), nestedReference.size());
    ASSERT_EQ(nestedAnimator.nodeGlobalTransforms.size(), 9u);
    for (size_t index = 0; index < nestedReference.size(); ++index)
        ExpectBoneBakeTestMatrix(nestedAnimator.boneMatrices[index], nestedReference[index]);

    system.Update(outerContext);

    const auto nextReference = SampleBoneBakeTestReference(*outerModel.skeleton, outerAnimator.clips[0], 1.0);
    for (size_t index = 0; index < nextReference.size(); ++index)
        ExpectBoneBakeTestMatrix(outerAnimator.boneMatrices[index], nextReference[index]);
}
TEST_F(BakedBoneAnimationTest, BakedDistanceLodHoldsPoseEventsAndFullPreviousPaletteForBothModes)
{
    for (const auto mode : {scene::BoneBakeMode::CPU_BAKED, scene::BoneBakeMode::GPU_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));
        auto clip=MakeBoneBakeTestSwim();clip.events={{0.1,"HeldOne",1,0},{0.17,"HeldTwo",2,0}};
        BoneBakeLodTestWorld world(mode,clip);
        std::vector<std::string> events;
        auto callback=std::make_unique<BoneBakeTestPoseCallback>();
        callback->onEvent=[&](const scene::AnimationEventInfo& event){events.push_back(event.name);};
        scene::ScriptEntry entry;entry.script=std::move(callback);
        world.actor->AddComponent<scene::ScriptComponent>().scripts.push_back(std::move(entry));
        world.Step();
        auto& animator=world.Animator();
        ASSERT_TRUE(animator.boneBake.active);ASSERT_TRUE(animator.poseEvaluatedThisFrame);
        const auto palette=animator.boneMatrices,globals=animator.nodeGlobalTransforms;
        const auto frame=animator.boneBake.frame;
        const auto center=animator.skinnedBoundsCenter;const float radius=animator.skinnedBoundsRadius;
        for (int held=1;held<=3;++held) {
            world.Step();
            EXPECT_FALSE(animator.poseEvaluatedThisFrame);EXPECT_EQ(animator.poseEvaluationCount,1u);
            EXPECT_TRUE(animator.boneBake.active);EXPECT_TRUE(animator.prevBoneMatricesValid);
            EXPECT_EQ(animator.boneBake.frame.first,frame.first);EXPECT_EQ(animator.boneBake.frame.second,frame.second);
            EXPECT_FLOAT_EQ(animator.boneBake.frame.alpha,frame.alpha);
            EXPECT_VEC3_NEAR(animator.skinnedBoundsCenter,center,0.0f);EXPECT_FLOAT_EQ(animator.skinnedBoundsRadius,radius);
            ASSERT_EQ(animator.boneMatrices.size(),palette.size());ASSERT_EQ(animator.nodeGlobalTransforms.size(),globals.size());
            for (size_t bone=0;bone<palette.size();++bone) {
                ExpectBoneBakeTestMatrix(animator.boneMatrices[bone],palette[bone],0);
                ExpectBoneBakeTestMatrix(animator.prevBoneMatrices[bone],palette[bone],0);
            }
            for (size_t node=0;node<globals.size();++node)ExpectBoneBakeTestMatrix(animator.nodeGlobalTransforms[node],globals[node],0);
            EXPECT_NEAR(animator.stateTime,(held+1)*0.0625f,0.00001f);
        }
        EXPECT_EQ(events,(std::vector<std::string>{"HeldOne","HeldTwo"}));
        world.Step();
        EXPECT_TRUE(animator.poseEvaluatedThisFrame);EXPECT_EQ(animator.poseEvaluationCount,2u);
        EXPECT_NEAR(animator.stateTime,0.3125f,0.00001f);
        const auto expected=SampleBoneBakeTestReference(*world.model.skeleton,clip,0.3125);
        for (size_t bone=0;bone<expected.size();++bone)ExpectBoneBakeTestMatrix(animator.boneMatrices[bone],expected[bone]);
        size_t currentWrites=0,previousWrites=0;
        const auto identity=math::Matrix4::Identity();
        for (const auto& write:world.renderer.writes) {
            if(write.buffer==0)++currentWrites;
            if(write.buffer!=1)continue;
            ++previousWrites;ASSERT_EQ(write.bytes.size(),8192u);
            EXPECT_EQ(std::memcmp(write.bytes.data(),palette.data(),palette.size()*sizeof(math::Matrix4)),0);
            for(size_t bone=palette.size();bone<128;++bone)
                EXPECT_EQ(std::memcmp(write.bytes.data()+bone*sizeof(identity),&identity,sizeof(identity)),0);
        }
        EXPECT_EQ(currentWrites,2u);EXPECT_EQ(previousWrites,4u);
        EXPECT_TRUE(world.scene.GetEntities<scene::BoneComponent>().empty());
    }
}

TEST_F(BakedBoneAnimationTest, BakedDistanceLodIsOptInAndNearestOrMissingCameraRefreshesImmediately)
{
    for (const auto mode : {scene::BoneBakeMode::CPU_BAKED, scene::BoneBakeMode::GPU_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));BoneBakeLodTestWorld world(mode);
        auto& animator=world.Animator();animator.poseLodEnabled=false;
        world.Step();world.Step();EXPECT_EQ(animator.poseEvaluationCount,2u);
        ASSERT_TRUE(animator.SetUpdateLod(true,1,2,0.125f,0.25f));
        world.Step();world.Step();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        auto& nearCamera=world.scene.CreateGameObject("Nearest camera");
        nearCamera.transform.position=world.actor->transform.position;
        nearCamera.AddComponent<scene::CameraComponent>();scene::FlushWorldTransforms(world.scene);
        world.Step();
        EXPECT_TRUE(animator.poseEvaluatedThisFrame);EXPECT_FALSE(animator.boneBake.active);
        EXPECT_EQ(animator.boneBake.fallback,scene::BoneBakeFallback::NEAR_CAMERA);
        EXPECT_EQ(world.scene.GetEntities<scene::BoneComponent>().size(),3u);
        nearCamera.GetComponent<scene::CameraComponent>()->enabled=false;
        world.Step();ASSERT_TRUE(animator.boneBake.active);world.Step();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        world.camera->GetComponent<scene::CameraComponent>()->enabled=false;
        world.Step();EXPECT_TRUE(animator.poseEvaluatedThisFrame);EXPECT_FALSE(animator.boneBake.active);
        EXPECT_EQ(animator.boneBake.fallback,scene::BoneBakeFallback::NEAR_CAMERA);
    }
}

TEST_F(BakedBoneAnimationTest, BakedDistanceLodRefreshesWhenCpuBonesRigLayersOrMorphBecomeRequired)
{
    for (const auto mode : {scene::BoneBakeMode::CPU_BAKED, scene::BoneBakeMode::GPU_SHARED}) {
        for(int reason=0;reason<4;++reason) {
            SCOPED_TRACE(static_cast<int>(mode));
            SCOPED_TRACE(reason);
            BoneBakeLodTestWorld world(mode);
            world.Step();world.Step();auto& animator=world.Animator();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
            const scene::BoneBakeFallback reasons[]={scene::BoneBakeFallback::CPU_BONES_REQUIRED,
                scene::BoneBakeFallback::RIG_CONTROL,scene::BoneBakeFallback::LAYERS_OR_MASK,scene::BoneBakeFallback::MORPH};
            if(reason==0)animator.boneBakeRequireCpuBones=true;
            if(reason==1)world.actor->AddComponent<scene::IKSolverComponent>();
            if(reason==2)animator.layers.emplace_back();
            if(reason==3)world.SkinnedRenderer().morphWeights["Smile"]=0.5f;
            world.Step();
            EXPECT_TRUE(animator.poseEvaluatedThisFrame);EXPECT_FALSE(animator.boneBake.active);
            EXPECT_EQ(animator.boneBake.fallback,reasons[reason]);
            EXPECT_EQ(world.scene.GetEntities<scene::BoneComponent>().size(),3u);
        }
    }
}

TEST_F(BakedBoneAnimationTest, BakedDistanceLodRevalidatesSampleRateAndModelWithTheSameSkeleton)
{
    for (const auto mode : {scene::BoneBakeMode::CPU_BAKED, scene::BoneBakeMode::GPU_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));BoneBakeLodTestWorld world(mode);
        world.Step();world.Step();auto& animator=world.Animator();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        const auto source=animator.boneBake.source;
        animator.boneBakeSampleRate=30;world.Step();
        ASSERT_TRUE(animator.poseEvaluatedThisFrame);ASSERT_TRUE(animator.boneBake.active);
        EXPECT_NE(animator.boneBake.source,source);EXPECT_FLOAT_EQ(animator.boneBake.sampleRate,30.0f);
        world.Step();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        auto replacement=MakeBoneBakeRuntimeTestModel();replacement.skeleton=std::move(world.model.skeleton);
        replacement.meshes.front()->cpuSkinnedVertices.front().position={30,0,0};
        const auto previousSource=animator.boneBake.source;
        world.SkinnedRenderer().model=&replacement;world.Step();
        ASSERT_TRUE(animator.poseEvaluatedThisFrame);ASSERT_TRUE(animator.boneBake.active);
        EXPECT_NE(animator.boneBake.source,previousSource);EXPECT_FLOAT_EQ(animator.boneBake.source->sourceMeshRadius,30.0f);
        world.Step();EXPECT_FALSE(animator.poseEvaluatedThisFrame);
        world.SkinnedRenderer().model=&world.model;
    }
}

TEST_F(BakedBoneAnimationTest, BakedDistanceLodRefreshesReimportAndSameStateClipReplacement)
{
    for (const auto mode : {scene::BoneBakeMode::CPU_BAKED, scene::BoneBakeMode::GPU_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));BoneBakeLodTestWorld world(mode);
        world.Step();world.Step();auto& animator=world.Animator();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        const auto oldSource=animator.boneBake.source;
        auto changed=MakeBoneBakeTestSwim();changed.tracks[0].positions[1].value.y=0.8f;
        ASSERT_TRUE(WriteBoneBakeTestClip(world.path,changed));ASSERT_EQ(asset::AssetManager::ReloadPath(world.path),1);
        world.Step();
        ASSERT_TRUE(animator.poseEvaluatedThisFrame);ASSERT_TRUE(animator.boneBake.active);
        EXPECT_NE(animator.boneBake.source,oldSource);EXPECT_FALSE(animator.prevBoneMatricesValid);
        const auto expected=SampleBoneBakeTestReference(*world.model.skeleton,changed,animator.stateTime);
        for(size_t bone=0;bone<expected.size();++bone)ExpectBoneBakeTestMatrix(animator.boneMatrices[bone],expected[bone]);
        world.Step();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        const auto reimported=animator.boneBake.source;
        const auto otherPath=world.temp.File("Assets/other.anim").generic_string();
        changed.name="Other";changed.tracks[0].positions[1].value.y=1.2f;
        ASSERT_TRUE(WriteBoneBakeTestClip(otherPath,changed));
        const auto handle=asset::AssetManager::Load<asset::AnimationClip>(otherPath);ASSERT_NE(asset::AssetManager::Get(handle),nullptr);
        animator.sharedClips.push_back({handle,{},0});animator.clipSourcePaths.push_back(otherPath);
        animator.states.front().sourcePath=otherPath;animator.states.front().clipName=changed.name;
        world.Step();
        EXPECT_EQ(animator.currentStateName,"Swim");EXPECT_TRUE(animator.poseEvaluatedThisFrame);
        ASSERT_TRUE(animator.boneBake.active);EXPECT_NE(animator.boneBake.source,reimported);
    }
}

TEST_F(BakedBoneAnimationTest, BakedDistanceLodKeepsTransitionsAndRootMotionCallbacksAtFullRate)
{
    for (const auto mode : {scene::BoneBakeMode::CPU_BAKED, scene::BoneBakeMode::GPU_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));BoneBakeLodTestWorld world(mode);
        auto& animator=world.Animator();
        auto next=animator.states.front();next.name="Next";animator.states.push_back(next);
        scene::AnimationTransition transition;transition.toStateName="Next";transition.hasExitTime=true;
        transition.exitTime=0.1f;transition.transitionDuration=0.125f;
        animator.states.front().transitions.push_back(transition);
        world.Step();world.Step();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        world.Step();ASSERT_FALSE(animator.poseEvaluatedThisFrame);
        world.Step();EXPECT_TRUE(animator.poseEvaluatedThisFrame);EXPECT_EQ(animator.blendToState,"Next");
        world.Step();EXPECT_TRUE(animator.poseEvaluatedThisFrame);
        world.Step();EXPECT_TRUE(animator.poseEvaluatedThisFrame);EXPECT_EQ(animator.currentStateName,"Next");
        int moves=0;auto callback=std::make_unique<BoneBakeTestPoseCallback>();callback->onMove=[&]{++moves;};
        scene::ScriptEntry entry;entry.script=std::move(callback);
        world.actor->AddComponent<scene::ScriptComponent>().scripts.push_back(std::move(entry));
        animator.rootMotion.mode=scene::RootMotionMode::ExtractOnly;
        for(int frame=0;frame<2;++frame){world.Step();EXPECT_TRUE(animator.poseEvaluatedThisFrame);}
        EXPECT_EQ(moves,2);
    }
}
TEST_F(BakedBoneAnimationTest, GpuPoseDefersCpuPalettesOnlyAfterSuccessfulGpuPreparation)
{
    BoneBakeLodTestWorld world(scene::BoneBakeMode::GPU_POSE_SHARED);
    world.Step(0.25f);
    auto& animator = world.Animator();
    ASSERT_TRUE(animator.boneBake.active);
    EXPECT_FALSE(animator.boneBake.cpuDeferred);
    const auto original = animator.boneMatrices;
    animator.boneBake.gpuReady = true;
    animator.boneBake.gpuOwner = world.resources.GetInstanceId();
    animator.boneBake.gpuEpoch = world.resources.GetResetVersion();
    world.renderer.writes.clear();
    world.Step(0.25f);
    ASSERT_TRUE(animator.boneBake.cpuDeferred);
    ASSERT_TRUE(animator.boneBake.previousPoseValid);
    ASSERT_EQ(animator.boneMatrices.size(), original.size());
    for (size_t bone = 0; bone < original.size(); ++bone) ExpectBoneBakeTestMatrix(animator.boneMatrices[bone], original[bone], 0);
    ASSERT_EQ(world.renderer.writes.size(), 1u);
    EXPECT_EQ(world.renderer.writes.front().bytes.size(), 8192u);
    world.renderer.writes.clear();
    world.Step(0.25f);
    ASSERT_TRUE(animator.boneBake.cpuDeferred);
    EXPECT_TRUE(world.renderer.writes.empty());
    const auto& bake = animator.boneBake;
    std::vector<math::Matrix4> expected, globals;
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(*bake.source, bake.frame, expected, globals,
        bake.target.get(), bake.target ? &bake.targetFrame : nullptr, bake.blendWeight), BoneBakeTestStatus::OK);
    ASSERT_TRUE(scene::MaterializeBoneBakeCpu(animator, world.resources, true));
    world.renderer.Drain();
    EXPECT_FALSE(animator.boneBake.cpuDeferred);
    ASSERT_EQ(animator.boneMatrices.size(), expected.size());
    for (size_t bone = 0; bone < expected.size(); ++bone) ExpectBoneBakeTestMatrix(animator.boneMatrices[bone], expected[bone]);
    ASSERT_EQ(world.renderer.writes.size(), 2u);
    for (const auto& write : world.renderer.writes) EXPECT_EQ(write.bytes.size(), 8192u);
}

TEST_F(BakedBoneAnimationTest, GpuPoseHoldSnapshotsDisplayedClockWithoutCpuUploads)
{
    BoneBakeLodTestWorld world(scene::BoneBakeMode::GPU_POSE_SHARED);
    world.Step(0.25f);
    auto& animator = world.Animator();
    animator.boneBake.gpuReady = true;
    animator.boneBake.gpuOwner = world.resources.GetInstanceId();
    animator.boneBake.gpuEpoch = world.resources.GetResetVersion();
    world.Step(0.25f);
    ASSERT_TRUE(animator.boneBake.cpuDeferred);
    const auto displayed = animator.boneBake.frame;
    world.renderer.writes.clear();
    world.Step(0.01f);
    EXPECT_FALSE(animator.poseEvaluatedThisFrame);
    EXPECT_TRUE(animator.boneBake.cpuDeferred);
    EXPECT_TRUE(animator.prevBoneMatricesValid);
    EXPECT_EQ(animator.boneBake.frame.first, displayed.first);
    EXPECT_EQ(animator.boneBake.previous.frame.first, displayed.first);
    EXPECT_FLOAT_EQ(animator.boneBake.previous.frame.alpha, displayed.alpha);
    EXPECT_TRUE(world.renderer.writes.empty());
}

TEST_F(BakedBoneAnimationTest, GpuPoseCpuReaderRestoresPreviousPaletteBeforeLegacyFallback)
{
    BoneBakeLodTestWorld world(scene::BoneBakeMode::GPU_POSE_SHARED);
    world.Step(0.25f);
    auto& animator = world.Animator();
    animator.boneBake.gpuReady = true;
    animator.boneBake.gpuOwner = world.resources.GetInstanceId();
    animator.boneBake.gpuEpoch = world.resources.GetResetVersion();
    world.Step(0.25f);
    ASSERT_TRUE(animator.boneBake.cpuDeferred);
    std::vector<math::Matrix4> previous, globals;
    const auto& bake = animator.boneBake;
    ASSERT_EQ(asset::EvaluateBakedBoneAnimation(*bake.source, bake.frame, previous, globals,
        bake.target.get(), bake.target ? &bake.targetFrame : nullptr, bake.blendWeight), BoneBakeTestStatus::OK);
    animator.boneBakeRequireCpuBones = true;
    world.Step(0.25f);
    EXPECT_FALSE(animator.boneBake.active);
    EXPECT_FALSE(animator.boneBake.cpuDeferred);
    EXPECT_TRUE(animator.prevBoneMatricesValid);
    ASSERT_EQ(animator.prevBoneMatrices.size(), previous.size());
    for (size_t bone = 0; bone < previous.size(); ++bone) ExpectBoneBakeTestMatrix(animator.prevBoneMatrices[bone], previous[bone]);
    EXPECT_FALSE(world.SkinnedRenderer().nodeEntities.empty());
}

TEST_F(BakedBoneAnimationTest, GpuPoseReimportKeepsPreviousHistoryInvalid)
{
    BoneBakeLodTestWorld world(scene::BoneBakeMode::GPU_POSE_SHARED);
    world.Step(0.25f);
    auto& animator = world.Animator();
    animator.boneBake.gpuReady = true;
    animator.boneBake.gpuOwner = world.resources.GetInstanceId();
    animator.boneBake.gpuEpoch = world.resources.GetResetVersion();
    world.Step(0.25f);
    ASSERT_TRUE(animator.boneBake.cpuDeferred);
    ASSERT_EQ(asset::AssetManager::ReloadPath(world.path), 1);
    world.Step(0.25f);
    EXPECT_TRUE(animator.boneBake.active);
    EXPECT_FALSE(animator.boneBake.cpuDeferred);
    EXPECT_FALSE(animator.prevBoneMatricesValid);
}

TEST_F(BakedBoneAnimationTest, RejectedBakeMeshCanRecoverAfterItsImportGenerationChanges)
{
    BoneBakeLodTestWorld world(scene::BoneBakeMode::GPU_POSE_SHARED);
    auto& vertex = world.model.meshes.front()->cpuSkinnedVertices.front();
    const auto original = vertex;
    vertex.boneWeights[0] = -1.0f;
    world.Step(0.25f);
    EXPECT_FALSE(world.Animator().boneBake.active);
    vertex = original;
    ASSERT_EQ(asset::AssetManager::ReloadPath(world.path), 1);
    world.Step(0.25f);
    EXPECT_TRUE(world.Animator().boneBake.active);
}

TEST_F(BakedBoneAnimationTest, GpuPoseCpuReturnSurvivesActorRetirementInTheResourceExecutor)
{
    BoneBakeLodTestWorld world(scene::BoneBakeMode::GPU_POSE_SHARED);
    world.Step(0.25f);
    auto& animator = world.Animator();
    animator.boneBake.gpuReady = true;
    animator.boneBake.gpuOwner = world.resources.GetInstanceId();
    animator.boneBake.gpuEpoch = world.resources.GetResetVersion();
    world.Step(0.25f);
    ASSERT_TRUE(animator.boneBake.cpuDeferred);
    animator.boneBakeRequireCpuBones = true;
    bool retired = false;
    world.renderer.beforeRecord = [&] { retired = true; world.scene.Clear(); };
    world.Step(0.25f);
    EXPECT_TRUE(retired);
    EXPECT_EQ(world.scene.GameObjectCount(), 0u);
}

TEST_F(BakedBoneAnimationTest, GpuPoseStableCpuFallbackKeepsTheOrdinaryDistanceLod)
{
    for (const auto mode : {scene::BoneBakeMode::LEGACY, scene::BoneBakeMode::GPU_POSE_SHARED}) {
        SCOPED_TRACE(static_cast<int>(mode));
        BoneBakeLodTestWorld world(mode);
        world.model.meshes.front()->cpuSkinnedVertices.front().boneWeights[0] = -1.0f;
        world.Step();
        auto& animator = world.Animator();
        ASSERT_FALSE(animator.boneBake.active);
        const auto matrices = animator.boneMatrices;
        const auto time = animator.stateTime;
        world.Step();
        EXPECT_FALSE(animator.poseEvaluatedThisFrame);
        EXPECT_GT(animator.stateTime, time);
        ASSERT_EQ(animator.boneMatrices.size(), matrices.size());
        for (size_t bone = 0; bone < matrices.size(); ++bone) ExpectBoneBakeTestMatrix(animator.boneMatrices[bone], matrices[bone], 0);
    }
}

} /// @note namespace fbzz::tests
