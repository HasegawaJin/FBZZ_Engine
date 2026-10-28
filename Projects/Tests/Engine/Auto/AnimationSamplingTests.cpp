/// @file    AnimationSamplingTests.cpp
/// @brief   キー列から 1 つの値を取り出す規則 (端点・補間・範囲外・キー 1 本) を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// .anim (AnimatorSystem) と .sequence (SequenceSystem) が同じ関数を共有している。
/// 補間が狂うと «アニメが 1 フレームだけ飛ぶ» «演出とキャラの動きが微妙にずれる» の
/// どちらかになり、原因を目で追えない。端点と中間を数値として固定する。
#include <TestKit/TestKit.hpp>
#include <Scene/Systems/AnimationWorkTrace.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/AnimationSampling.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <Engine/Scene/Systems/AnimatorSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <cmath>
#include <vector>

namespace fbzz::tests {
namespace {

std::vector<asset::VectorKey> VectorRamp()
{
    return { { 0.0, { 0.0f, 0.0f, 0.0f } }, { 10.0, { 10.0f, 20.0f, -30.0f } } };
}

std::vector<asset::FloatKey> FloatRamp()
{
    asset::FloatKey a;
    a.time  = 0.0;
    a.value = 0.0f;
    asset::FloatKey b;
    b.time  = 10.0;
    b.value = 100.0f;
    return { a, b };
}

} // namespace

class AnimationSamplingTest : public testkit::EngineFixture {};

/// @name 空・1 本

TEST_F(AnimationSamplingTest, EmptyKeysFallBackToTheGivenValue)
{
    /// @note キーが無いチャンネルは «触らない» が正。0 を返すとボーンが原点へ吸われる。
    const math::Vector3 fallback{ 1.0f, 2.0f, 3.0f };

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys({}, 5.0, fallback), fallback,
                     testkit::kTolerance);
    EXPECT_QUAT_NEAR(asset::SampleQuaternionKeys({}, 5.0,
                                                 math::Quaternion::Identity()),
                     math::Quaternion::Identity(), testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, ASingleKeyHoldsItsValueForever)
{
    const std::vector<asset::VectorKey> single{ { 5.0, { 7.0f, 8.0f, 9.0f } } };

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(single, -100.0, math::Vector3::ZERO),
                     math::Vector3(7.0f, 8.0f, 9.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(single, 100.0, math::Vector3::ZERO),
                     math::Vector3(7.0f, 8.0f, 9.0f), testkit::kTolerance);
}

/// @name 端点と範囲外

TEST_F(AnimationSamplingTest, ReturnsTheEndpointsExactly)
{
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 0.0, math::Vector3::ZERO),
                     math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 10.0, math::Vector3::ZERO),
                     math::Vector3(10.0f, 20.0f, -30.0f), testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, ClampsOutsideTheKeyRange)
{
    /// @note 尺の外を «ループの先頭» や «外挿» にしない。繰り返しは呼び出し側の仕事。
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, -50.0, math::Vector3::ZERO),
                     math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 50.0, math::Vector3::ZERO),
                     math::Vector3(10.0f, 20.0f, -30.0f), testkit::kTolerance);
}

/// @name 補間

TEST_F(AnimationSamplingTest, InterpolatesLinearlyBetweenTwoKeys)
{
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 5.0, math::Vector3::ZERO),
                     math::Vector3(5.0f, 10.0f, -15.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(asset::SampleVectorKeys(keys, 2.5, math::Vector3::ZERO),
                     math::Vector3(2.5f, 5.0f, -7.5f), testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, StepInterpolationHoldsThePreviousKey)
{
    /// @note 表情の切り替えやフラグは «途中の値» を作ってはいけない。
    const auto keys = VectorRamp();

    EXPECT_VEC3_NEAR(
        asset::SampleVectorKeys(keys, 9.99, math::Vector3::ZERO, asset::AnimInterp::Step),
        math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, PicksTheRightSegmentAmongManyKeys)
{
    /// @note upper_bound の境界。1 つずれると «常に前のキーの値» になる。
    const std::vector<asset::VectorKey> keys{
        { 0.0, { 0.0f, 0.0f, 0.0f } },
        { 1.0, { 10.0f, 0.0f, 0.0f } },
        { 2.0, { 20.0f, 0.0f, 0.0f } },
        { 3.0, { 30.0f, 0.0f, 0.0f } },
    };

    EXPECT_NEAR(asset::SampleVectorKeys(keys, 1.5, math::Vector3::ZERO).x, 15.0f,
                testkit::kTolerance);
    EXPECT_NEAR(asset::SampleVectorKeys(keys, 2.0, math::Vector3::ZERO).x, 20.0f,
                testkit::kTolerance);
    EXPECT_NEAR(asset::SampleVectorKeys(keys, 2.5, math::Vector3::ZERO).x, 25.0f,
                testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, SurvivesTwoKeysAtTheSameTime)
{
    /// @note 書き出し側が同じ時刻へ 2 本置くことがある。0 除算で NaN を返さないこと。
    const std::vector<asset::VectorKey> keys{
        { 0.0, { 0.0f, 0.0f, 0.0f } },
        { 1.0, { 10.0f, 0.0f, 0.0f } },
        { 1.0, { 20.0f, 0.0f, 0.0f } },
        { 2.0, { 30.0f, 0.0f, 0.0f } },
    };

    const math::Vector3 sampled = asset::SampleVectorKeys(keys, 1.0, math::Vector3::ZERO);

    EXPECT_FALSE(std::isnan(sampled.x));
}

/// @name クォータニオン

TEST_F(AnimationSamplingTest, SlerpsRotationsAndKeepsThemNormalised)
{
    const std::vector<asset::QuaternionKey> keys{
        { 0.0, math::Quaternion::Identity() },
        { 10.0, math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)) },
    };

    const math::Quaternion mid =
        asset::SampleQuaternionKeys(keys, 5.0, math::Quaternion::Identity());

    EXPECT_QUAT_NEAR(mid,
                     math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f)),
                     testkit::kLooseTolerance);
    EXPECT_NEAR(mid.Length(), 1.0f, testkit::kLooseTolerance);
}

TEST_F(AnimationSamplingTest, RotationStepHoldsThePreviousKey)
{
    const std::vector<asset::QuaternionKey> keys{
        { 0.0, math::Quaternion::Identity() },
        { 10.0, math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f)) },
    };

    EXPECT_QUAT_NEAR(asset::SampleQuaternionKeys(keys, 9.0, math::Quaternion::Identity(),
                                                 asset::AnimInterp::Step),
                     math::Quaternion::Identity(), testkit::kTolerance);
}

/// @name スカラー

TEST_F(AnimationSamplingTest, SamplesFloatsLinearly)
{
    const auto keys = FloatRamp();

    EXPECT_NEAR(asset::SampleFloatKeys(keys, 5.0, asset::AnimInterp::Linear), 50.0f,
                testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, EmptyFloatKeysGiveZero)
{
    EXPECT_NEAR(asset::SampleFloatKeys({}, 5.0, asset::AnimInterp::Linear), 0.0f,
                testkit::kTolerance);
}

TEST_F(AnimationSamplingTest, CubicMatchesLinearAtTheKeysThemselves)
{
    /// @note 接線が何であれ、キー上の値はキーの値。ここがずれると «キーを打った瞬間だけ飛ぶ»。
    const auto keys = FloatRamp();

    EXPECT_NEAR(asset::SampleFloatKeys(keys, 0.0, asset::AnimInterp::Cubic), 0.0f,
                testkit::kLooseTolerance);
    EXPECT_NEAR(asset::SampleFloatKeys(keys, 10.0, asset::AnimInterp::Cubic), 100.0f,
                testkit::kLooseTolerance);
}

TEST_F(AnimationSamplingTest, CubicWithFlatTangentsStaysInsideTheRange)
{
    /// @note 接線 0 の Hermite は行き過ぎない。オーバーシュートすると
    ///       «クリップに無いポーズ» が 1 フレームだけ出る。
    const auto keys = FloatRamp();

    for (int step = 0; step <= 20; ++step) {
        const double ticks = static_cast<double>(step) * 0.5;
        const float value = asset::SampleFloatKeys(keys, ticks, asset::AnimInterp::Cubic);

        EXPECT_GE(value, -testkit::kLooseTolerance) << "ticks=" << ticks;
        EXPECT_LE(value, 100.0f + testkit::kLooseTolerance) << "ticks=" << ticks;
    }
}


/// @note Observation metadata must never attach raw counts to an unfinalized or discontinuous source frame.
TEST_F(AnimationSamplingTest, ObservationRequiresFinalizedMatchingSourceIdentity)
{
    scene::AnimationWorkRow row; row.BeginSource(10, 20, 30);
    EXPECT_FALSE(row.HasCompleteSourceIdentity()); EXPECT_FALSE(row.MatchesCompleteSource(10, 20, 30));
    row.FinalizeSource(10, 20, 30); EXPECT_TRUE(row.MatchesCompleteSource(10, 20, 30));
    EXPECT_FALSE(row.MatchesCompleteSource(11, 20, 30)); EXPECT_FALSE(row.MatchesCompleteSource(10, 21, 30));
    EXPECT_FALSE(row.MatchesCompleteSource(10, 20, 31));
    for (int dimension = 0; dimension < 3; ++dimension) {
        scene::AnimationWorkRow changed; changed.BeginSource(10, 20, 30); changed.poseRefreshEntered = 7;
        changed.FinalizeSource(dimension == 0 ? 11 : 10, dimension == 1 ? 21 : 20, dimension == 2 ? 31 : 30);
        EXPECT_TRUE(changed.sourceIdentityMismatch); EXPECT_FALSE(changed.HasCompleteSourceIdentity());
        EXPECT_FALSE(changed.MatchesCompleteSource(changed.resourceFrame, changed.sceneGeneration, changed.resourceEpoch));
        EXPECT_EQ(changed.poseRefreshEntered, 7u);
    }
}
TEST_F(AnimationSamplingTest, ObservationMismatchCannotBeHiddenByRepeatedFinalizationOrLegacyColumnMutation)
{
    scene::AnimationWorkRow row; row.BeginSource(10, 20, 30); row.FinalizeSource(11, 20, 30);
    row.FinalizeSource(10, 20, 30); EXPECT_TRUE(row.sourceIdentityMismatch); EXPECT_FALSE(row.HasCompleteSourceIdentity());
    scene::AnimationWorkRow coherent; coherent.BeginSource(10, 20, 30); coherent.FinalizeSource(10, 20, 30);
    coherent.resourceFrame = 11; EXPECT_FALSE(coherent.HasCompleteSourceIdentity()); EXPECT_FALSE(coherent.MatchesCompleteSource(11, 20, 30));
}
TEST_F(AnimationSamplingTest, ObservationRefreshEntryDoesNotClaimClipEvaluationOrBoneWork)
{
    scene::AnimationWorkRow row;
    row.CountPoseDecision(true, true, false); row.CountPoseDecision(true, true, true); row.CountPoseDecision(false, false, true);
    EXPECT_EQ(row.posePathActors, 3u); EXPECT_EQ(row.poseLodGatePassed, 2u); EXPECT_EQ(row.poseReducedIntervalActors, 2u);
    EXPECT_EQ(row.poseRefreshEntered, 2u); EXPECT_EQ(row.poseHeld, 1u); EXPECT_EQ(row.heldCurrentPaletteUpdates, 1u);
    EXPECT_EQ(row.posePathActors, row.poseRefreshEntered + row.poseHeld);
    EXPECT_EQ(row.poseLegacyFkCalls, 0u); EXPECT_EQ(row.poseBakedSuccesses, 0u); EXPECT_EQ(row.legacyEvaluatedBoneWrites, 0u);
    row.bindPaletteUpdateCalls = 1; row.bindPaletteApiBytes = 8192; row.poseBindFallbackActors = 1;
    EXPECT_EQ(row.poseLegacyFkCalls, 0u); EXPECT_EQ(row.poseBakedSuccesses, 0u); EXPECT_EQ(row.legacyEvaluatedBoneWrites, 0u);
}
TEST_F(AnimationSamplingTest, ObservationDefaultsAreUnavailableAndObservedZeroIsRepresentable)
{
    scene::AnimationWorkRow row;
    EXPECT_FALSE(row.poseCountersAvailable); EXPECT_FALSE(row.legacyBoneWritesAvailable); EXPECT_FALSE(row.paletteApiCountersAvailable);
    row.poseCountersAvailable = true; row.legacyBoneWritesAvailable = true; row.paletteApiCountersAvailable = true;
    EXPECT_EQ(row.posePathActors, 0u); EXPECT_EQ(row.legacyEvaluatedBoneWrites, 0u);
    EXPECT_EQ(row.previousPaletteApiBytes + row.currentPaletteApiBytes + row.bindPaletteApiBytes, 0u);
    EXPECT_TRUE(row.poseCountersAvailable); EXPECT_FALSE(row.HasCompleteSourceIdentity());
}

namespace {
struct ActorTraceTestWrite { uint32_t buffer=0; std::vector<uint8_t> bytes; };
class ActorTraceTestBuffer final : public renderer::IConstantBuffer {
public:
    ActorTraceTestBuffer(size_t size,uint32_t buffer,std::vector<ActorTraceTestWrite>& writes):m_size(size),m_buffer(buffer),m_writes(writes) {}
    void Update(const void* data,size_t size) override {
        ActorTraceTestWrite write; write.buffer=m_buffer; write.bytes.resize(size);
        std::memcpy(write.bytes.data(),data,size); m_writes.push_back(std::move(write));
    }
    size_t GetSize() const override { return m_size; }
private:
    size_t m_size; uint32_t m_buffer; std::vector<ActorTraceTestWrite>& m_writes;
};
class ActorTraceTestRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&,renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&,renderer::ResourceManager&) override {}
    void Resize(uint32_t,uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>,renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t,uint32_t,uint32_t,uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override { return std::make_unique<ActorTraceTestBuffer>(size,nextBuffer++,writes); }
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
    bool IsRenderThread() const override { return rendering; }
    void ExecuteResourceOperation(const std::function<void()>& operation) override { const bool old=rendering; rendering=true; operation(); rendering=old; }
    bool RecordResourceUpdate(std::function<void()> operation) override { ExecuteResourceOperation(operation); return true; }
    std::vector<ActorTraceTestWrite> writes;
private:
    uint32_t nextBuffer=0; bool rendering=false;
};
std::string ActorTraceTestEnv(const char* name)
{
#ifdef _MSC_VER
    char* raw=nullptr; size_t length=0;
    if(_dupenv_s(&raw,&length,name)!=0 || !raw)return {};
    const std::unique_ptr<char,decltype(&std::free)> owned(raw,&std::free); return owned.get();
#else
    const char* value=std::getenv(name); return value?value:"";
#endif
}
std::vector<std::string> ActorTraceTestColumns(const std::string& line)
{
    std::vector<std::string> columns; std::istringstream input(line); std::string column;
    while(std::getline(input,column,','))columns.push_back(column);
    return columns;
}
}
/// @note OFF validates real snapshot/update behavior; ON CSV assertions require a fresh process with warmup=0 and samples=3.
TEST_F(AnimationSamplingTest, ActorCollectorIntegrationPreservesFirstAndSecondHeldSnapshotUpdates)
{
    struct RestoreTimeFrame { uint64_t saved=Time::frameCount; ~RestoreTimeFrame(){Time::frameCount=saved;} } restore;
    ActorTraceTestRenderer renderer; renderer::ResourceManager resources(renderer);
    asset::Model model; model.skeleton=std::make_unique<asset::Skeleton>();
    const auto a=math::Matrix4::Identity(), b=math::Matrix4::Translate({1.0f,0.0f,0.0f});
    model.skeleton->bones.resize(1); model.skeleton->referencePose={b};
    scene::Scene worldScene; physics::World physicsWorld;
    worldScene.CreateGameObject("Camera").AddComponent<scene::CameraComponent>();
    auto& object=worldScene.CreateGameObject("Actor"); object.transform.worldPosition={100.0f,0.0f,0.0f};
    object.AddComponent<scene::SkinnedMeshRenderer>().model=&model;
    auto& animator=object.AddComponent<scene::AnimatorComponent>();
    scene::AnimationState state; state.name="Hold"; state.clipIndex=0; animator.states.push_back(state);
    animator.currentStateName=animator.defaultStateName=animator.poseStateName="Hold";
    asset::AnimationClip clip; clip.name="Hold"; clip.durationSeconds=clip.durationTicks=1.0; clip.ticksPerSecond=1.0; animator.clips.push_back(clip);
    animator.clipsLoaded=true; animator.appliedAssetGeneration=asset::AssetManager::GetAssetGeneration();
    animator.rootMotion.mode=scene::RootMotionMode::None; animator.poseLodEnabled=true;
    animator.poseLodNearDistance=1.0f; animator.poseLodFarDistance=2.0f; animator.poseLodFarInterval=1.0f;
    animator.poseUpdateElapsed=1.0f; animator.poseSkeleton=model.skeleton.get();
    animator.boneMatrices={a}; animator.prevBoneMatrices={a}; animator.prevBoneMatricesValid=true;
    scene::AnimatorSystem system; SystemContext context{worldScene,physicsWorld,&resources,nullptr,0.01f,0.01f,true,true};
    const bool collectorOn=scene::AnimationActorTraceRequested();
    if(collectorOn) {
        ASSERT_EQ(ActorTraceTestEnv("FBZZ_ANIMATION_ACTOR_TRACE_WARMUP"),"0");
        ASSERT_EQ(ActorTraceTestEnv("FBZZ_ANIMATION_ACTOR_TRACE_SAMPLES"),"3");
    }
    for(uint32_t frame=0;frame<3;++frame) {
        resources.AdvanceFrame(); Time::frameCount=restore.saved+frame+1; system.Update(context);
        EXPECT_EQ(animator.poseEvaluatedThisFrame,frame==0);
        ASSERT_EQ(animator.boneMatrices.size(),1u);
        EXPECT_EQ(std::memcmp(&animator.boneMatrices[0],&b,sizeof(b)),0);
    }
    std::vector<ActorTraceTestWrite> previousWrites;
    for(const auto& write:renderer.writes)if(write.buffer==1)previousWrites.push_back(write);
    ASSERT_EQ(previousWrites.size(),3u);
    for(uint32_t frame=0;frame<3;++frame) {
        ASSERT_EQ(previousWrites[frame].bytes.size(),8192u);
        const auto& expected=frame==0?a:b;
        EXPECT_EQ(std::memcmp(previousWrites[frame].bytes.data(),&expected,sizeof(expected)),0);
        for(uint32_t bone=1;bone<128;++bone)EXPECT_EQ(std::memcmp(previousWrites[frame].bytes.data()+bone*sizeof(a),&a,sizeof(a)),0);
    }
    if(collectorOn) {
        const auto output=ActorTraceTestEnv("FBZZ_ANIMATION_ACTOR_TRACE_OUTPUT");
        std::ifstream input(std::filesystem::u8path(output+".actor.csv")); ASSERT_TRUE(input.good());
        std::string line; ASSERT_TRUE(static_cast<bool>(std::getline(input,line))); const auto header=ActorTraceTestColumns(line);
        const auto field=[&](const std::vector<std::string>& row,const std::string& name) {
            const auto found=std::find(header.begin(),header.end(),name);
            if(found==header.end())return std::string("missing"); const size_t index=static_cast<size_t>(found-header.begin());
            return index<row.size()?row[index]:std::string("missing");
        };
        for(uint32_t frame=0;frame<3;++frame) {
            ASSERT_TRUE(static_cast<bool>(std::getline(input,line))); const auto row=ActorTraceTestColumns(line);
            EXPECT_EQ(field(row,"snapshotGuardPassed"),"1"); EXPECT_EQ(field(row,"desiredEqualityAvailable"),frame==1?"0":"1");
            EXPECT_EQ(field(row,"previousValidBeforeSnapshot"),frame==1?"0":"1");
            EXPECT_EQ(field(row,"previousHistoryInvalidatedAfterSnapshot"),frame==0?"1":"0");
            EXPECT_EQ(field(row,"desiredEqual"),frame==1?"0":"1");
            EXPECT_EQ(field(row,"actualRefresh"),frame==0?"1":"0"); EXPECT_EQ(field(row,"actorComplete"),"1");
            EXPECT_EQ(field(row,"heldSuffixLowerBound"),std::to_string(frame));
            EXPECT_EQ(field(row,"heldStreakExactAvailable"),"1");
            EXPECT_EQ(field(row,"consecutiveHeldAtLeast2"),frame==2?"1":"0");
            EXPECT_EQ(field(row,"cpuSameDesiredPotential"),frame==2?"1":"0");
            EXPECT_EQ(field(row,"actualNativeSkipAvailable"),"0"); EXPECT_EQ(field(row,"actualNativeSkip"),"");
        }
        EXPECT_FALSE(static_cast<bool>(std::getline(input,line)));
    }
}
/// @note ON exercises the real collector overlap/finalization boundary in a fresh process with warmup=0 and samples=1.
TEST_F(AnimationSamplingTest, ActorCollectorIntegrationRejectsNestedFrameAndRemovedComponent)
{
    ActorTraceTestRenderer renderer; renderer::ResourceManager resources(renderer);
    scene::Scene worldScene; auto& object=worldScene.CreateGameObject("Actor"); object.AddComponent<scene::AnimatorComponent>();
    const auto entity=object.GetID(); const auto lifetime=worldScene.GetComponentLifetime<scene::AnimatorComponent>(entity);
    ASSERT_NE(lifetime,0u);
    if(scene::AnimationActorTraceRequested()) {
        ASSERT_EQ(ActorTraceTestEnv("FBZZ_ANIMATION_ACTOR_TRACE_WARMUP"),"0");
        ASSERT_EQ(ActorTraceTestEnv("FBZZ_ANIMATION_ACTOR_TRACE_SAMPLES"),"1");
        scene::AnimationActorFrame frame(true,resources.FrameStamp(),worldScene.GetRenderSceneGeneration(),resources.GetResetVersion(),resources.GetInstanceId());
        ASSERT_TRUE(frame.Active()); frame.SetPopulation(1);
        auto* row=frame.BeginActor(entity.index,entity.generation,lifetime,0.01f,1.0f); ASSERT_NE(row,nullptr);
        scene::AnimationActorScope outer(row);
        {
            scene::AnimationActorFrame nested(true,resources.FrameStamp(),worldScene.GetRenderSceneGeneration(),resources.GetResetVersion(),resources.GetInstanceId());
            EXPECT_FALSE(nested.Active()); scene::AnimationActorScope inactive(nullptr); EXPECT_EQ(scene::CurrentAnimationActorRow(),nullptr);
        }
        EXPECT_EQ(scene::CurrentAnimationActorRow(),row);
        worldScene.RemoveComponent<scene::AnimatorComponent>(entity);
        frame.FinishActor(row,worldScene,resources.FrameStamp(),worldScene.GetRenderSceneGeneration(),resources.GetResetVersion(),resources.GetInstanceId());
        frame.FinalizeSource(worldScene,resources.FrameStamp(),worldScene.GetRenderSceneGeneration(),resources.GetResetVersion(),resources.GetInstanceId());
        EXPECT_TRUE(row->afterRunObserved); EXPECT_FALSE(row->afterRunIdentityValid);
        EXPECT_FALSE(row->frameEndIdentityValid); EXPECT_FALSE(row->sourceComplete); EXPECT_FALSE(row->Complete());
    } else worldScene.RemoveComponent<scene::AnimatorComponent>(entity);
    EXPECT_EQ(worldScene.GetComponentLifetime<scene::AnimatorComponent>(entity),0u);
}
TEST_F(AnimationSamplingTest, ActorPaletteEqualityDistinguishesFirstAndSecondHeldAfterRefresh)
{
    const auto a=math::Matrix4::Identity(), b=math::Matrix4::Translate({1.0f,0.0f,0.0f});
    std::vector<math::Matrix4> previous{a}, current{a};
    EXPECT_TRUE(scene::animation_actor_detail::CompareEffectivePalettes(current,previous).equal);
    previous=current; current={b};
    EXPECT_FALSE(scene::animation_actor_detail::CompareEffectivePalettes(current,previous).equal);
    previous=current;
    EXPECT_TRUE(scene::animation_actor_detail::CompareEffectivePalettes(current,previous).equal);
}
TEST_F(AnimationSamplingTest, ActorPaletteEqualityComparesIdentityTailsAndClampsAt128)
{
    std::vector<math::Matrix4> small{math::Matrix4::Identity()}, padded(128,math::Matrix4::Identity());
    EXPECT_TRUE(scene::animation_actor_detail::CompareEffectivePalettes(small,padded).equal);
    padded[127].m[0][3]=1.0f;
    EXPECT_FALSE(scene::animation_actor_detail::CompareEffectivePalettes(small,padded).equal);
    padded[127]=math::Matrix4::Identity(); padded.push_back(math::Matrix4::Zero());
    const auto result=scene::animation_actor_detail::CompareEffectivePalettes(small,padded);
    EXPECT_TRUE(result.equal); EXPECT_EQ(result.currentClampedCount,1u); EXPECT_EQ(result.previousClampedCount,128u);
    EXPECT_EQ(result.compareArgumentBytes,128u*sizeof(math::Matrix4));
}
TEST_F(AnimationSamplingTest, ActorPaletteEqualityPreservesSignedZeroAndNanPayloadBits)
{
    std::vector<math::Matrix4> a{math::Matrix4::Identity()}, b=a;
    a[0].m[0][1]=0.0f; b[0].m[0][1]=-0.0f;
    EXPECT_FALSE(scene::animation_actor_detail::CompareEffectivePalettes(a,b).equal);
    const uint32_t nanA=0x7fc00001u,nanB=0x7fc00002u;
    std::memcpy(&a[0].m[0][1],&nanA,sizeof(nanA)); std::memcpy(&b[0].m[0][1],&nanA,sizeof(nanA));
    EXPECT_TRUE(scene::animation_actor_detail::CompareEffectivePalettes(a,b).equal);
    std::memcpy(&b[0].m[0][1],&nanB,sizeof(nanB));
    EXPECT_FALSE(scene::animation_actor_detail::CompareEffectivePalettes(a,b).equal);
}
TEST_F(AnimationSamplingTest, ActorHeldHistoryNeedsObservedFreshForExactStreak)
{
    using namespace scene::animation_actor_detail;
    HeldHistory history;
    const auto make=[](uint64_t frame,bool refresh) {
        ActorRow row; row.key.componentLifetime=1; row.resourceFrame=row.timeFrame=row.updateOrdinal=frame;
        row.afterRunObserved=row.afterRunIdentityValid=row.frameEndIdentityValid=row.sourceComplete=true;
        row.actualDecisionAvailable=true; row.actualRefresh=refresh; row.snapshotObserved=row.snapshotGuardPassed=true;
        row.desiredEqualityAvailable=row.desiredEqual=true; return row;
    };
    auto initial=make(1,false); history.Observe(initial);
    EXPECT_EQ(initial.heldSuffixLowerBound,1u); EXPECT_FALSE(initial.heldStreakExactAvailable); EXPECT_FALSE(initial.previousHeldAvailable);
    auto suffix=make(2,false); history.Observe(suffix);
    EXPECT_EQ(suffix.heldSuffixLowerBound,2u); EXPECT_FALSE(suffix.heldStreakExactAvailable); EXPECT_TRUE(suffix.consecutiveHeldAtLeast2);
    auto fresh=make(3,true); history.Observe(fresh); EXPECT_TRUE(fresh.heldStreakExactAvailable); EXPECT_EQ(fresh.heldSuffixLowerBound,0u);
    auto first=make(4,false); first.desiredEqual=false; history.Observe(first);
    EXPECT_EQ(first.heldSuffixLowerBound,1u); EXPECT_TRUE(first.heldStreakExactAvailable); EXPECT_FALSE(first.consecutiveHeldAtLeast2); EXPECT_FALSE(first.cpuSameDesiredPotential);
    auto second=make(5,false); history.Observe(second);
    EXPECT_EQ(second.heldSuffixLowerBound,2u); EXPECT_TRUE(second.heldStreakExactAvailable); EXPECT_TRUE(second.consecutiveHeldAtLeast2); EXPECT_TRUE(second.cpuSameDesiredPotential);
    auto gap=make(7,false); history.Observe(gap);
    EXPECT_EQ(gap.heldSuffixLowerBound,1u); EXPECT_FALSE(gap.heldStreakExactAvailable); EXPECT_FALSE(gap.cpuSameDesiredPotentialAvailable);
    auto invalid=make(8,false); invalid.frameEndIdentityValid=false; history.Observe(invalid);
    EXPECT_FALSE(invalid.consecutiveHeldAtLeast2Available); EXPECT_FALSE(invalid.cpuSameDesiredPotentialAvailable);
    auto recovery=make(9,false); history.Observe(recovery); EXPECT_FALSE(recovery.previousHeldAvailable); EXPECT_FALSE(recovery.heldStreakExactAvailable);
}
TEST_F(AnimationSamplingTest, ActorInvalidDuplicateAndUnknownDecisionCutHistory)
{
    using namespace scene::animation_actor_detail;
    HeldHistory history; ActorRow fresh;
    fresh.key.componentLifetime=1; fresh.resourceFrame=fresh.timeFrame=fresh.updateOrdinal=1;
    fresh.afterRunObserved=fresh.afterRunIdentityValid=fresh.frameEndIdentityValid=fresh.sourceComplete=true;
    fresh.actualDecisionAvailable=fresh.actualRefresh=true; auto observedFresh=fresh; history.Observe(observedFresh);
    auto duplicate=fresh; duplicate.resourceFrame=duplicate.timeFrame=duplicate.updateOrdinal=2; duplicate.actualRefresh=false; duplicate.duplicateActor=true;
    history.Observe(duplicate); EXPECT_FALSE(duplicate.Complete()); EXPECT_FALSE(history.valid);
    auto unknown=fresh; unknown.resourceFrame=unknown.timeFrame=unknown.updateOrdinal=3; unknown.actualDecisionAvailable=false;
    history.Observe(unknown); EXPECT_FALSE(history.valid); EXPECT_FALSE(unknown.heldStreakExactAvailable);
    ActorKey oldKey,newKey; oldKey.componentLifetime=1; newKey=oldKey; newKey.componentLifetime=2; EXPECT_FALSE(oldKey==newKey);
    newKey=oldKey; newKey.sceneGeneration=1; EXPECT_FALSE(oldKey==newKey);
    newKey=oldKey; newKey.resourceEpoch=1; EXPECT_FALSE(oldKey==newKey);
    newKey=oldKey; newKey.previousHandleGeneration=1; EXPECT_FALSE(oldKey==newKey);
}
TEST_F(AnimationSamplingTest, ActorCapacityRefusesOverflowWithoutReplacingAcceptedRows)
{
    using namespace scene::animation_actor_detail;
    uint32_t count=0,index=123;
    EXPECT_TRUE(ClaimBoundedSlot(count,2,index)); EXPECT_EQ(index,0u);
    EXPECT_TRUE(ClaimBoundedSlot(count,2,index)); EXPECT_EQ(index,1u);
    EXPECT_FALSE(ClaimBoundedSlot(count,2,index)); EXPECT_EQ(count,2u); EXPECT_EQ(index,1u);
    EXPECT_GT(uint64_t{1217}*FRAME_CAPACITY,ACTOR_CAPACITY);
    EXPECT_LT(uint64_t{1217}*120,ACTOR_CAPACITY);
}
TEST_F(AnimationSamplingTest, ActorIdentityReplacementResetAndFrameGapsDoNotExtendHeldStreak)
{
    using namespace scene::animation_actor_detail;
    const auto fresh=[] {
        ActorRow r; r.key={10,20,30,40,50,60,70,80}; r.resourceFrame=r.timeFrame=r.updateOrdinal=1;
        r.afterRunObserved=r.afterRunIdentityValid=r.frameEndIdentityValid=r.sourceComplete=true;
        r.actualDecisionAvailable=r.actualRefresh=true; r.snapshotObserved=r.snapshotGuardPassed=true;
        r.desiredEqualityAvailable=r.desiredEqual=true; return r;
    };
    for(uint32_t dimension=0;dimension<12;++dimension) {
        HeldHistory history; auto first=fresh(); history.Observe(first);
        auto held=first; held.actualRefresh=false; held.resourceFrame=held.timeFrame=held.updateOrdinal=2;
        switch(dimension) {
        case 0: ++held.key.sceneGeneration; break;
        case 1: ++held.key.resourceEpoch; break;
        case 2: ++held.key.resourceInstance; break;
        case 3: ++held.key.componentLifetime; break;
        case 4: ++held.key.entityIndex; break;
        case 5: ++held.key.entityGeneration; break;
        case 6: ++held.key.previousHandleId; break;
        case 7: ++held.key.previousHandleGeneration; break;
        case 8: ++held.resourceFrame; break;
        case 9: ++held.timeFrame; break;
        case 10: ++held.updateOrdinal; break;
        case 11: held.previousHistoryInvalidatedAfterSnapshot=true; break;
        }
        history.Observe(held);
        EXPECT_FALSE(held.previousHeldAvailable) << dimension;
        EXPECT_FALSE(held.heldStreakExactAvailable) << dimension;
        EXPECT_FALSE(held.cpuSameDesiredPotentialAvailable) << dimension;
        EXPECT_EQ(held.heldSuffixLowerBound,1u) << dimension;
    }
}
TEST_F(AnimationSamplingTest, ActorNullScopeShadowsActiveTlsAndRestores)
{
    scene::animation_actor_detail::ActorRow row;
    EXPECT_EQ(scene::CurrentAnimationActorRow(),nullptr);
    { scene::AnimationActorScope inactive(nullptr); EXPECT_EQ(scene::CurrentAnimationActorRow(),nullptr); }
    {
        scene::AnimationActorScope outer(&row); EXPECT_EQ(scene::CurrentAnimationActorRow(),&row);
        { scene::AnimationActorScope inactive(nullptr); EXPECT_EQ(scene::CurrentAnimationActorRow(),nullptr); }
        EXPECT_EQ(scene::CurrentAnimationActorRow(),&row);
    }
    EXPECT_EQ(scene::CurrentAnimationActorRow(),nullptr);
}
TEST_F(AnimationSamplingTest, ActorCaptureOwnershipRejectsReentryAndReleasesOnlyOwnedLease)
{
    using namespace scene::animation_actor_detail;
    std::atomic_flag ownership=ATOMIC_FLAG_INIT;
    EXPECT_TRUE(TryClaimCapture(ownership));
    EXPECT_FALSE(TryClaimCapture(ownership));
    EXPECT_FALSE(TryClaimCapture(ownership));
    ReleaseCapture(ownership);
    EXPECT_TRUE(TryClaimCapture(ownership));
    ReleaseCapture(ownership);
}

} // namespace fbzz::tests

namespace fbzz::tests {

namespace {
using PhaseKeyType=scene::animation_phase_detail::Key;
using PhaseSourceType=scene::animation_phase_detail::Source;
PhaseSourceType PhaseTestSource(uint64_t frame)
{
    return {frame,frame,11,22,33};
}
PhaseKeyType PhaseTestKey()
{
    for(uint64_t lifetime=1;lifetime<65536;++lifetime) {
        PhaseKeyType key{lifetime,7,9};
        const float credit=scene::animation_phase_detail::InitialCredit(key,0.25f);
        if(credit>0.15f && credit<0.20f)return key;
    }
    return {};
}
}
TEST_F(AnimationSamplingTest, PhaseCreditIsDeterministicBoundedAndLifetimeSpecific)
{
    using namespace scene::animation_phase_detail;
    const auto key=PhaseTestKey();ASSERT_NE(key.componentLifetime,0u);
    const float credit=InitialCredit(key,0.25f);EXPECT_GT(credit,0.15f);EXPECT_LT(credit,0.20f);
    EXPECT_EQ(InitialCredit(key,0.25f),credit);EXPECT_FLOAT_EQ(InitialCredit(key,0.5f),credit*2);
    auto replacement=key;++replacement.componentLifetime;EXPECT_NE(HashKey(replacement),HashKey(key));
    EXPECT_FLOAT_EQ(InitialCredit({},0.25f),0);EXPECT_FLOAT_EQ(InitialCredit(key,0),0);
    EXPECT_FLOAT_EQ(InitialCredit(key,2),0);EXPECT_FLOAT_EQ(InitialCredit(key,std::numeric_limits<float>::quiet_NaN()),0);
    EXPECT_FALSE(CreditDue(true,0.20f,0.25f,credit));EXPECT_FALSE(CreditDue(false,0,0.25f,0));
    EXPECT_FALSE(CreditDue(false,0.1f,0.25f,0.25f));EXPECT_FALSE(CreditDue(false,-1,0.25f,credit));
}
TEST_F(AnimationSamplingTest, PhaseCreditRequiresSuccessfulLegacyAndConsumesAnyRefreshOnce)
{
    using namespace scene::animation_phase_detail;Ledger ledger(2);const auto key=PhaseTestKey();float credit=-1;
    auto ticket=ledger.Prepare(key,PhaseTestSource(1),0.25f,true);
    EXPECT_TRUE(ticket.canSeed);EXPECT_EQ(ticket.credit,0);EXPECT_FALSE(CreditDue(false,0.05f,0.25f,ticket.credit));
    EXPECT_FALSE(ledger.Complete(ticket,{true,false},true,credit));EXPECT_EQ(credit,0);
    ticket=ledger.Prepare(key,PhaseTestSource(2),0.25f,true);
    EXPECT_TRUE(ledger.Complete(ticket,{true,true},true,credit));EXPECT_EQ(credit,InitialCredit(key,0.25f));
    ticket=ledger.Prepare(key,PhaseTestSource(3),0.25f,true);EXPECT_FALSE(ticket.canSeed);EXPECT_EQ(ticket.credit,credit);
    EXPECT_TRUE(CreditDue(false,0.10f,0.25f,ticket.credit));
    // A forced fallback refresh also consumes pending credit; it cannot re-seed the issued lifetime.
    EXPECT_FALSE(ledger.Complete(ticket,{true,false},true,credit));
    ticket=ledger.Prepare(key,PhaseTestSource(4),0.25f,true);EXPECT_EQ(ticket.credit,0);EXPECT_FALSE(ticket.canSeed);
    EXPECT_FALSE(ledger.Complete(ticket,{true,true},true,credit));
}
TEST_F(AnimationSamplingTest, PhaseCreditInvalidationKeepsIssuedLifetimeMarker)
{
    using namespace scene::animation_phase_detail;const auto key=PhaseTestKey();
    for(uint32_t reason=0;reason<7;++reason) {
        Ledger ledger(1);float credit=0;auto ticket=ledger.Prepare(key,PhaseTestSource(1),0.25f,true);
        ASSERT_TRUE(ledger.Complete(ticket,{true,true},true,credit));auto source=PhaseTestSource(2);float interval=0.25f;bool eligible=true;
        if(reason==0)source.resourceFrame=3;else if(reason==1)source.timeFrame=3;
        else if(reason==2)++source.sceneGeneration;else if(reason==3)++source.resourceEpoch;
        else if(reason==4)++source.resourceInstance;else if(reason==5)interval=0.5f;else eligible=false;
        ticket=ledger.Prepare(key,source,interval,eligible);EXPECT_EQ(ticket.credit,0);EXPECT_FALSE(ticket.canSeed);
        EXPECT_FALSE(ledger.Complete(ticket,{true,true},eligible,credit));
        ticket=ledger.Prepare(key,PhaseTestSource(4),0.25f,true);EXPECT_EQ(ticket.credit,0);EXPECT_FALSE(ticket.canSeed);
    }
}
TEST_F(AnimationSamplingTest, PhaseCreditCancelDuplicateAndStaleTicketCannotReseed)
{
    using namespace scene::animation_phase_detail;Ledger ledger(1);const auto key=PhaseTestKey();float credit=0;
    auto first=ledger.Prepare(key,PhaseTestSource(1),0.25f,true);auto nested=ledger.Prepare(key,PhaseTestSource(1),0.25f,true);
    EXPECT_FALSE(ledger.Complete(first,{true,true},true,credit));EXPECT_TRUE(ledger.Complete(nested,{true,true},true,credit));
    ledger.Cancel(key);EXPECT_FALSE(ledger.Complete(nested,{true,true},true,credit));
    auto next=ledger.Prepare(key,PhaseTestSource(2),0.25f,true);EXPECT_EQ(next.credit,0);EXPECT_FALSE(next.canSeed);
    EXPECT_FALSE(ledger.Complete(next,{true,true},false,credit));
}
TEST_F(AnimationSamplingTest, PhaseCreditSourceConfigAndRendererReplacementInvalidateWithoutReseed)
{
    using namespace scene::animation_phase_detail;const auto key=PhaseTestKey();
    for(uint32_t change=0;change<12;++change) {
        Ledger ledger(1);float credit=0;auto source=PhaseTestSource(1);
        source.rendererLifetime=100;source.rendererIndex=2;source.rendererGeneration=3;source.modelIdentity=400;source.skeletonIdentity=500;source.assetGeneration=600;
        auto ticket=ledger.Prepare(key,source,.25f,true);ASSERT_TRUE(ledger.Complete(ticket,{true,true},true,credit));
        ++source.resourceFrame;++source.timeFrame;
        if(change==0)++source.rendererLifetime;else if(change==1)++source.rendererIndex;else if(change==2)++source.rendererGeneration;
        else if(change==3)++source.modelIdentity;else if(change==4)++source.skeletonIdentity;else if(change==5)++source.assetGeneration;
        else source.lodSettings[change-6]=1;
        ticket=ledger.Prepare(key,source,.25f,true);EXPECT_EQ(ticket.credit,0);EXPECT_FALSE(ticket.canSeed);
        EXPECT_FALSE(ledger.Complete(ticket,{true,true},true,credit));
    }
    // Real component replacement gets a fresh global lifetime even when the Entity key stays the same.
    scene::Scene worldScene;auto& object=worldScene.CreateGameObject("Actor");object.AddComponent<scene::AnimatorComponent>();
    object.AddComponent<scene::SkinnedMeshRenderer>();const auto id=object.GetID();
    const auto animatorLife=worldScene.GetComponentLifetime<scene::AnimatorComponent>(id);
    const auto rendererLife=worldScene.GetComponentLifetime<scene::SkinnedMeshRenderer>(id);
    object.RemoveComponent<scene::SkinnedMeshRenderer>();EXPECT_EQ(worldScene.GetComponentLifetime<scene::SkinnedMeshRenderer>(id),0u);
    object.AddComponent<scene::SkinnedMeshRenderer>();EXPECT_NE(worldScene.GetComponentLifetime<scene::SkinnedMeshRenderer>(id),rendererLife);
    EXPECT_EQ(worldScene.GetComponentLifetime<scene::AnimatorComponent>(id),animatorLife);
    object.RemoveComponent<scene::AnimatorComponent>();EXPECT_EQ(worldScene.GetComponentLifetime<scene::AnimatorComponent>(id),0u);
    object.AddComponent<scene::AnimatorComponent>();EXPECT_NE(worldScene.GetComponentLifetime<scene::AnimatorComponent>(id),animatorLife);
}
TEST_F(AnimationSamplingTest, PhaseCreditReplacementCloneAndCapacityFallbackDoNotEvict)
{
    using namespace scene::animation_phase_detail;Ledger ledger(2);const auto key=PhaseTestKey();float credit=0;
    auto first=ledger.Prepare(key,PhaseTestSource(1),0.25f,true);ASSERT_TRUE(ledger.Complete(first,{true,true},true,credit));
    auto clone=key;++clone.componentLifetime;auto second=ledger.Prepare(clone,PhaseTestSource(2),0.25f,true);
    EXPECT_EQ(second.credit,0);EXPECT_TRUE(second.canSeed);ASSERT_TRUE(ledger.Complete(second,{true,true},true,credit));
    auto replacement=clone;++replacement.componentLifetime;auto refused=ledger.Prepare(replacement,PhaseTestSource(3),0.25f,true);
    EXPECT_FALSE(refused.tracked);EXPECT_TRUE(refused.capacityRejected);EXPECT_EQ(refused.credit,0);
    ledger.Cancel(key);auto retained=ledger.Prepare(key,PhaseTestSource(4),0.25f,true);
    EXPECT_TRUE(retained.tracked);EXPECT_FALSE(retained.canSeed);EXPECT_EQ(retained.credit,0);
    Ledger invalid(0);EXPECT_TRUE(invalid.Prepare(key,PhaseTestSource(1),0.25f,true).capacityRejected);
}
TEST_F(AnimationSamplingTest, PhaseCreditCadenceHasOneEarlyCycleThenOriginalResetZero)
{
    using namespace scene::animation_phase_detail;const auto key=PhaseTestKey();
    // The variable dt case models the measured .25s cohort; these are source arithmetic, not performance measurements.
    const std::vector<float> varied={.041f,.046f,.039f,.050f,.044f,.043f,.038f,.047f,.049f,.040f,.045f,.042f};
    for(bool variable:{false,true}) {
        Ledger ledger(1);float legacyElapsed=0,phaseElapsed=0,credit=0;uint32_t legacyCount=1,phaseCount=1;
        auto first=ledger.Prepare(key,PhaseTestSource(1),.25f,true);ASSERT_TRUE(ledger.Complete(first,{true,true},true,credit));
        uint32_t firstEarly=0,lastRefresh=0;bool consumed=false;
        for(uint32_t frame=1;frame<=120;++frame) {
            const float dt=variable?varied[(frame-1)%varied.size()]:.05f;
            legacyElapsed+=dt;phaseElapsed+=dt;const bool normal=phaseElapsed+math::EPSILON>=.25f;
            auto ticket=ledger.Prepare(key,PhaseTestSource(frame+1),.25f,true);
            const bool early=CreditDue(normal,phaseElapsed,.25f,ticket.credit);
            const bool refresh=normal||early;
            if(legacyElapsed+math::EPSILON>=.25f){legacyElapsed=0;++legacyCount;}
            if(early){EXPECT_FALSE(consumed);firstEarly=frame;consumed=true;}
            if(refresh) {
                if(lastRefresh && !variable)EXPECT_EQ(frame-lastRefresh,5u);
                phaseElapsed=0;lastRefresh=frame;++phaseCount;
            }
            EXPECT_FALSE(ledger.Complete(ticket,{refresh,refresh},true,credit));
            if(consumed)EXPECT_EQ(credit,0);
            if(frame==3){EXPECT_EQ(legacyCount,1u);EXPECT_EQ(phaseCount,2u);}
        }
        EXPECT_GT(firstEarly,0u);EXPECT_LT(firstEarly,5u);
        EXPECT_GE(phaseCount,legacyCount);EXPECT_LE(phaseCount,legacyCount+1u);
        if(!variable){EXPECT_EQ(phaseCount,25u);EXPECT_EQ(legacyCount,25u);}
        // No residual credit or overshoot is carried after the one early refresh.
        auto end=ledger.Prepare(key,PhaseTestSource(122),.25f,true);EXPECT_EQ(end.credit,0);EXPECT_FALSE(end.canSeed);
    }
}
/// @note Real AnimatorSystem, live valid hierarchy, and full native buffer writes. Run fresh with mode=0 and mode=1 separately; trace collectors stay OFF.
TEST_F(AnimationSamplingTest, PhaseCreditIntegrationPreservesInitialPoseAndFullPreviousPalette)
{
    using namespace scene::animation_phase_detail;
    ASSERT_FALSE(scene::AnimationActorTraceRequested());ASSERT_EQ(ActorTraceTestEnv("FBZZ_ANIMATION_TRACE_OUTPUT"),"");
    struct RestoreTimeFrame {uint64_t saved=Time::frameCount;~RestoreTimeFrame(){Time::frameCount=saved;}}restore;
    ActorTraceTestRenderer renderer;renderer::ResourceManager resources(renderer);scene::Scene worldScene;physics::World physicsWorld;
    asset::Model model;model.skeleton=std::make_unique<asset::Skeleton>();auto& skeleton=*model.skeleton;
    skeleton.rootNodeIndex=0;skeleton.nodes.resize(1);skeleton.nodes[0].name="Root";skeleton.nodes[0].boneIndex=0;
    skeleton.bones.resize(1);skeleton.bones[0].name="Root";skeleton.bones[0].nodeIndex=0;skeleton.referencePose={math::Matrix4::Identity()};
    worldScene.CreateGameObject("Camera").AddComponent<scene::CameraComponent>();
    auto& object=worldScene.CreateGameObject("Actor");object.transform.worldPosition={100,0,0};
    auto& mesh=object.AddComponent<scene::SkinnedMeshRenderer>();mesh.model=&model;
    auto& bone=worldScene.CreateGameObject("Root");bone.SetParent(&object);auto& component=bone.AddComponent<scene::BoneComponent>();
    component.nodeIndex=component.boneIndex=0;component.skinnedMeshEntity=object.GetID();mesh.nodeEntities={bone.GetID()};mesh.skeletonRootEntity=bone.GetID();
    scene::AnimatorComponent* animator=nullptr;Key key;
    for(uint32_t attempt=0;attempt<128;++attempt) {
        if(animator)object.RemoveComponent<scene::AnimatorComponent>();animator=&object.AddComponent<scene::AnimatorComponent>();
        key={worldScene.GetComponentLifetime<scene::AnimatorComponent>(object.GetID()),object.GetID().index,object.GetID().generation};
        if(InitialCredit(key,.25f)>.12f && InitialCredit(key,.25f)<.20f)break;
    }
    ASSERT_GT(InitialCredit(key,.25f),.12f);ASSERT_LT(InitialCredit(key,.25f),.20f);scene::AnimationState state;state.name="Hold";state.clipIndex=0;animator->states.push_back(state);
    animator->currentStateName=animator->defaultStateName=animator->poseStateName="Hold";
    asset::AnimationClip clip;clip.name="Hold";clip.durationSeconds=clip.durationTicks=1;clip.ticksPerSecond=1;animator->clips.push_back(clip);
    animator->clipsLoaded=true;animator->appliedAssetGeneration=asset::AssetManager::GetAssetGeneration();animator->rootMotion.mode=scene::RootMotionMode::None;
    animator->poseLodEnabled=true;animator->poseLodNearDistance=1;animator->poseLodFarDistance=2;animator->poseLodFarInterval=.25f;
    animator->poseUpdateElapsed=.25f;animator->poseSkeleton=&skeleton;animator->boneMatrices={math::Matrix4::Identity()};
    scene::AnimatorSystem system;SystemContext context{worldScene,physicsWorld,&resources,nullptr,.05f,.05f,true,true};
    const bool enabled=scene::AnimationPhaseDistributionEnabled();float elapsed=.25f,pending=0;uint64_t expectedCount=0;
    uint32_t firstNext=0,last=0;
    for(uint32_t frame=0;frame<24;++frame) {
        elapsed+=.05f;const bool normal=elapsed+math::EPSILON>=.25f;const bool expected=normal||CreditDue(normal,elapsed,.25f,pending);
        resources.AdvanceFrame();Time::frameCount=restore.saved+frame+1;system.Update(context);
        EXPECT_EQ(animator->poseEvaluatedThisFrame,expected);
        if(expected){++expectedCount;elapsed=0;if(frame==0)pending=enabled?InitialCredit(key,.25f):0;else pending=0;
            if(frame && !firstNext)firstNext=frame;if(last)EXPECT_EQ(frame-last,5u);last=frame;}
        EXPECT_EQ(animator->poseEvaluationCount,expectedCount);EXPECT_FLOAT_EQ(animator->poseUpdateElapsed,elapsed);
        EXPECT_EQ(animator->boneMatrices.size(),1u);EXPECT_EQ(animator->nodeGlobalTransforms.size(),1u);EXPECT_FALSE(animator->boneBake.active);
    }
    EXPECT_EQ(firstNext,enabled?static_cast<uint32_t>(std::ceil((.25f-InitialCredit(key,.25f)-math::EPSILON)/.05f)):5u);
    uint32_t previous=0,current=0;const auto identity=math::Matrix4::Identity();
    for(const auto& write:renderer.writes) {
        if(write.buffer==1)++previous;else if(write.buffer==0)++current;else ADD_FAILURE()<<"unexpected native buffer";
        ASSERT_EQ(write.bytes.size(),8192u);for(uint32_t index=1;index<128;++index)EXPECT_EQ(std::memcmp(write.bytes.data()+index*sizeof(identity),&identity,sizeof(identity)),0);
    }
    EXPECT_EQ(previous,24u);EXPECT_EQ(current,expectedCount);
}

/// @note Real pending-credit invalidations: same-value LOD setter stays immediate, child rig and renderer replacement cancel credit permanently for this lifetime.
TEST_F(AnimationSamplingTest, PhaseCreditIntegrationRigRendererAndSameValueLodInvalidations)
{
    for(uint32_t reason=0;reason<3;++reason) {
    using namespace scene::animation_phase_detail;
    ASSERT_FALSE(scene::AnimationActorTraceRequested());ASSERT_EQ(ActorTraceTestEnv("FBZZ_ANIMATION_TRACE_OUTPUT"),"");
    struct RestoreTimeFrame {uint64_t saved=Time::frameCount;~RestoreTimeFrame(){Time::frameCount=saved;}}restore;
    ActorTraceTestRenderer renderer;renderer::ResourceManager resources(renderer);scene::Scene worldScene;physics::World physicsWorld;
    asset::Model model;model.skeleton=std::make_unique<asset::Skeleton>();auto& skeleton=*model.skeleton;
    skeleton.rootNodeIndex=0;skeleton.nodes.resize(1);skeleton.nodes[0].name="Root";skeleton.nodes[0].boneIndex=0;
    skeleton.bones.resize(1);skeleton.bones[0].name="Root";skeleton.bones[0].nodeIndex=0;skeleton.referencePose={math::Matrix4::Identity()};
    worldScene.CreateGameObject("Camera").AddComponent<scene::CameraComponent>();
    auto& object=worldScene.CreateGameObject("Actor");object.transform.worldPosition={100,0,0};
    auto& mesh=object.AddComponent<scene::SkinnedMeshRenderer>();mesh.model=&model;
    auto& bone=worldScene.CreateGameObject("Root");bone.SetParent(&object);auto& component=bone.AddComponent<scene::BoneComponent>();
    component.nodeIndex=component.boneIndex=0;component.skinnedMeshEntity=object.GetID();mesh.nodeEntities={bone.GetID()};mesh.skeletonRootEntity=bone.GetID();
    scene::AnimatorComponent* animator=nullptr;Key key;
    for(uint32_t attempt=0;attempt<128;++attempt) {
        if(animator)object.RemoveComponent<scene::AnimatorComponent>();animator=&object.AddComponent<scene::AnimatorComponent>();
        key={worldScene.GetComponentLifetime<scene::AnimatorComponent>(object.GetID()),object.GetID().index,object.GetID().generation};
        if(InitialCredit(key,.25f)>.12f && InitialCredit(key,.25f)<.20f)break;
    }
    ASSERT_GT(InitialCredit(key,.25f),.12f);ASSERT_LT(InitialCredit(key,.25f),.20f);scene::AnimationState state;state.name="Hold";state.clipIndex=0;animator->states.push_back(state);
    animator->currentStateName=animator->defaultStateName=animator->poseStateName="Hold";
    asset::AnimationClip clip;clip.name="Hold";clip.durationSeconds=clip.durationTicks=1;clip.ticksPerSecond=1;animator->clips.push_back(clip);
    animator->clipsLoaded=true;animator->appliedAssetGeneration=asset::AssetManager::GetAssetGeneration();animator->rootMotion.mode=scene::RootMotionMode::None;
    animator->poseLodEnabled=true;animator->poseLodNearDistance=1;animator->poseLodFarDistance=2;animator->poseLodFarInterval=.25f;
    animator->poseUpdateElapsed=.25f;animator->poseSkeleton=&skeleton;animator->boneMatrices={math::Matrix4::Identity()};

        scene::AnimatorSystem system;SystemContext context{worldScene,physicsWorld,&resources,nullptr,.05f,.05f,true,true};
        resources.AdvanceFrame();Time::frameCount=restore.saved+1;system.Update(context);
        ASSERT_TRUE(animator->poseEvaluatedThisFrame);ASSERT_EQ(animator->poseEvaluationCount,1u);ASSERT_FLOAT_EQ(animator->poseUpdateElapsed,0);
        if(reason==0)ASSERT_TRUE(animator->SetUpdateLod(true,1,2,animator->poseLodMidInterval,.25f));
        else if(reason==1)bone.AddComponent<scene::IKSolverComponent>();
        else {
            object.RemoveComponent<scene::SkinnedMeshRenderer>();auto& replacement=object.AddComponent<scene::SkinnedMeshRenderer>();
            replacement.model=&model;replacement.nodeEntities={bone.GetID()};replacement.skeletonRootEntity=bone.GetID();
        }
        uint64_t expectedCount=1;
        for(uint32_t frame=1;frame<12;++frame) {
            resources.AdvanceFrame();Time::frameCount=restore.saved+frame+1;system.Update(context);
            const bool expected=reason==0?(frame==1 || (frame-1)%5==0):frame%5==0;
            EXPECT_EQ(animator->poseEvaluatedThisFrame,expected);if(expected)++expectedCount;
            EXPECT_EQ(animator->poseEvaluationCount,expectedCount);
            if(reason==1 && frame==1)bone.RemoveComponent<scene::IKSolverComponent>();
        }
    }
}

} // namespace fbzz::tests
