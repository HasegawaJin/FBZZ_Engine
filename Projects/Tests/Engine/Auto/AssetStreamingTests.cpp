/// @file    AssetStreamingTests.cpp
/// @brief   非同期アセット要求台帳の重複集約・キャンセル・世代照合・依存・転送完了待ちを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
///
/// ジョブも GPU 転送の完了もテストが手で進める。スレッド・sleep・実時間には依存しない。
/// @see Docs/design/asset-streaming.md «導入順と受け入れ条件»
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/AssetStreamChannel.hpp>
#include <Engine/Asset/AssetStreaming.hpp>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

using asset::AssetLoadError;
using asset::AssetLoadState;
using asset::AssetStreamer;

/// @brief テスト専用の型。`AssetStore<StreamTestAsset>` はテスト exe 側の実体を使う。
struct StreamTestAsset {
    std::string         value;
    asset::AssetQuality quality = 0;
};

struct StreamTestDecoded final : asset::IDecodedAsset {
    std::string         value;
    asset::AssetQuality quality = 0;
    std::size_t         bytes = 0;
    [[nodiscard]] std::size_t CpuBytes() const override { return bytes; }
};

struct StreamTestContext final : asset::AssetJobContext {
    int snapshotSequence = 0;
};

/// @brief 振る舞いを台本で決める経路。Decode は投入時の Snapshot 番号を値に埋める。
class ScriptedChannel final : public asset::AssetStreamChannel<StreamTestAsset> {
public:
    struct Script {
        bool fail = false;
        std::vector<asset::AssetDependency> dependencies;
        /// 最高品質のときの常駐バイト数。品質段 1 つごとに半分にする。
        std::size_t bytes = 10;
    };

    std::unordered_map<std::string, Script> scripts;
    /// true なら BeginUpload が非 0 のトークンを返し、completedTokens に入るまで完了しない。
    bool                    asyncUpload = false;
    std::set<std::uint64_t> completedTokens;
    std::uint64_t           deviceEpoch = 1;
    bool                    qualitySupported = false;
    bool                    refuseEviction = false;
    int                     snapshots = 0;
    int                     uploads = 0;
    int                     discards = 0;
    int                     discardsAfterDeviceLoss = 0;
    int                     evictions = 0;

    [[nodiscard]] std::string MakeKey(const std::string& reference) override { return reference; }

    [[nodiscard]] bool SupportsQuality() const override { return qualitySupported; }
    [[nodiscard]] asset::AssetQuality LowestQuality() const override { return 3; }

    [[nodiscard]] bool Evict(asset::RawAssetHandle handle, const std::string& key) override
    {
        if (refuseEviction) return false;
        const bool evicted = asset::AssetStreamChannel<StreamTestAsset>::Evict(handle, key);
        if (evicted) ++evictions;
        return evicted;
    }

    /// @brief 同期ロードの代わり。値を "now" にして埋める。
    [[nodiscard]] bool LoadImmediately(asset::RawAssetHandle handle, const std::string&) override
    {
        auto asset = std::make_unique<StreamTestAsset>();
        asset->value = "now";
        return asset::AssetStore<StreamTestAsset>::Get().Replace(Typed(handle), std::move(asset));
    }

    [[nodiscard]] bool Snapshot(const std::string& key, asset::AssetJobInput& out, std::string&) override
    {
        auto context = std::make_shared<StreamTestContext>();
        context->snapshotSequence = ++snapshots;
        out.resolvedPath = key;
        out.context = std::move(context);
        return true;
    }

    [[nodiscard]] asset::AssetDecodeResult Decode(const asset::AssetJobInput& input) override
    {
        asset::AssetDecodeResult result;
        const Script script = scripts.count(input.key) ? scripts.at(input.key) : Script{};
        if (script.fail) {
            result.error = AssetLoadError::DecodeFailed;
            result.message = "scripted failure";
            return result;
        }
        auto decoded = std::make_unique<StreamTestDecoded>();
        const auto& context = static_cast<const StreamTestContext&>(*input.context);
        decoded->value = input.key + "#" + std::to_string(context.snapshotSequence);
        decoded->quality = input.quality;
        decoded->bytes = script.bytes >> input.quality;
        decoded->dependencies = script.dependencies;
        result.decoded = std::move(decoded);
        return result;
    }

    [[nodiscard]] std::uint64_t DeviceEpoch() const override { return deviceEpoch; }

    [[nodiscard]] std::unique_ptr<asset::IAssetCandidate> BeginUpload(
        asset::IDecodedAsset& decoded, std::uint64_t& outToken, std::string&) override
    {
        auto candidate = std::make_unique<asset::AssetCandidate<StreamTestAsset>>();
        candidate->asset = std::make_unique<StreamTestAsset>();
        candidate->asset->value = static_cast<StreamTestDecoded&>(decoded).value;
        candidate->asset->quality = static_cast<StreamTestDecoded&>(decoded).quality;
        outToken = asyncUpload ? static_cast<std::uint64_t>(++uploads) : 0;
        if (!asyncUpload) ++uploads;
        return candidate;
    }

    [[nodiscard]] bool IsUploadComplete(std::uint64_t token) const override
    {
        return token == 0 || completedTokens.count(token) != 0;
    }

    void Discard(asset::IAssetCandidate&, bool deviceStillValid) override
    {
        ++discards;
        if (!deviceStillValid) ++discardsAfterDeviceLoss;
    }
};

class AssetStreamingTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        asset::AssetStore<StreamTestAsset>::Get().Clear();
        m_channel = std::make_shared<ScriptedChannel>();
        CreateStreamer(AssetStreamer::Config{});
    }

    void TearDown() override
    {
        m_streamer.reset();
        asset::AssetStore<StreamTestAsset>::Get().Clear();
        EngineFixture::TearDown();
    }

    void CreateStreamer(AssetStreamer::Config config)
    {
        m_streamer = std::make_unique<AssetStreamer>(
            [this](std::function<void()> job) { m_jobs.push_back(std::move(job)); }, config);
        m_streamer->RegisterChannel(m_channel);
    }

    /// @brief 溜まったジョブを投入順に全部実行する。
    void RunAllJobs()
    {
        std::vector<std::function<void()>> jobs;
        jobs.swap(m_jobs);
        for (auto& job : jobs) job();
    }

    /// @brief index 番目のジョブだけ実行して取り除く。
    void RunJob(std::size_t index)
    {
        ASSERT_LT(index, m_jobs.size());
        auto job = std::move(m_jobs[index]);
        m_jobs.erase(m_jobs.begin() + static_cast<std::ptrdiff_t>(index));
        job();
    }

    [[nodiscard]] AssetLoadState StateOf(const asset::AssetLease<StreamTestAsset>& lease) const
    {
        return m_streamer->GetStatus(lease.Handle()).state;
    }

    /// @brief 要求して公開まで進める。
    [[nodiscard]] asset::AssetLease<StreamTestAsset> LoadReady(const std::string& key,
                                                               const asset::AssetRequestOptions& options = {})
    {
        auto lease = m_streamer->Request<StreamTestAsset>(key, options);
        m_streamer->Pump();
        RunAllJobs();
        m_streamer->Pump();
        return lease;
    }

    void PumpTimes(int count)
    {
        for (int i = 0; i < count; ++i) { m_streamer->Pump(); RunAllJobs(); }
    }

    std::shared_ptr<ScriptedChannel>     m_channel;
    std::unique_ptr<AssetStreamer>       m_streamer;
    std::vector<std::function<void()>>   m_jobs;
};

} // namespace

/// @name 受付と重複集約

TEST_F(AssetStreamingTest, RequestReservesAHandleWithoutRunningAnyJob)
{
    const auto lease = m_streamer->Request<StreamTestAsset>("A");

    EXPECT_TRUE(lease.Handle().IsValid());
    EXPECT_EQ(StateOf(lease), AssetLoadState::Queued);
    EXPECT_EQ(m_streamer->TryGet(lease.Handle()), nullptr);
    EXPECT_TRUE(m_jobs.empty());
}

TEST_F(AssetStreamingTest, DuplicateRequestsShareOneHandleAndOneJob)
{
    const auto first = m_streamer->Request<StreamTestAsset>("A");
    const auto second = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();

    EXPECT_EQ(first.Handle(), second.Handle());
    EXPECT_EQ(m_jobs.size(), 1u);
}

TEST_F(AssetStreamingTest, PublishesAtTheFrameBoundaryAfterTheJobCompletes)
{
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    EXPECT_EQ(StateOf(lease), AssetLoadState::Decoding);

    RunAllJobs();
    EXPECT_EQ(m_streamer->TryGet(lease.Handle()), nullptr) << "Pump 前に公開してはいけない";

    m_streamer->Pump();
    ASSERT_NE(m_streamer->TryGet(lease.Handle()), nullptr);
    EXPECT_EQ(m_streamer->TryGet(lease.Handle())->value, "A#1");
    EXPECT_EQ(StateOf(lease), AssetLoadState::Ready);
}

TEST_F(AssetStreamingTest, UnregisteredTypeIsRejectedWithAReason)
{
    auto lease = m_streamer->Request<int>("A");

    EXPECT_FALSE(lease.Handle().IsValid());
    EXPECT_EQ(lease.RequestError(), AssetLoadError::NoChannel);
}

TEST_F(AssetStreamingTest, LedgerCapacityRejectsNewKeysWithBudgetExceeded)
{
    AssetStreamer::Config config;
    config.maxEntries = 1;
    CreateStreamer(config);

    const auto first = m_streamer->Request<StreamTestAsset>("A");
    const auto again = m_streamer->Request<StreamTestAsset>("A");
    const auto second = m_streamer->Request<StreamTestAsset>("B");

    EXPECT_TRUE(first.Handle().IsValid());
    EXPECT_TRUE(again.Handle().IsValid()) << "既存キーへの要求は枠を使わない";
    EXPECT_FALSE(second.Handle().IsValid());
    EXPECT_EQ(second.RequestError(), AssetLoadError::BudgetExceeded);
}

TEST_F(AssetStreamingTest, JobsInFlightAreBoundedAndHigherPriorityGoesFirst)
{
    AssetStreamer::Config config;
    config.maxJobsInFlight = 1;
    CreateStreamer(config);

    asset::AssetRequestOptions background;
    background.priority = asset::AssetPriority::Background;
    asset::AssetRequestOptions required;
    required.priority = asset::AssetPriority::Required;
    const auto low = m_streamer->Request<StreamTestAsset>("Low", background);
    const auto high = m_streamer->Request<StreamTestAsset>("High", required);
    m_streamer->Pump();

    EXPECT_EQ(m_jobs.size(), 1u);
    EXPECT_EQ(StateOf(high), AssetLoadState::Decoding);
    EXPECT_EQ(StateOf(low), AssetLoadState::Queued);
}

/// @name キャンセルと古い結果の棄却

TEST_F(AssetStreamingTest, ReleasingOneOfTwoLeasesKeepsTheLoadGoing)
{
    auto first = m_streamer->Request<StreamTestAsset>("A");
    const auto second = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    first.Release();

    RunAllJobs();
    m_streamer->Pump();

    EXPECT_EQ(StateOf(second), AssetLoadState::Ready);
}

TEST_F(AssetStreamingTest, ReleasingEveryLeaseCancelsAndDropsTheLateResult)
{
    auto lease = m_streamer->Request<StreamTestAsset>("A");
    const auto handle = lease.Handle();
    m_streamer->Pump();
    lease.Release();

    EXPECT_EQ(m_streamer->GetStatus(handle).state, AssetLoadState::None) << "解放済みハンドルは知らないもの";
    EXPECT_FALSE(asset::AssetStore<StreamTestAsset>::Get().IsLive(handle)) << "予約スロットは返す";

    RunAllJobs();
    m_streamer->Pump();

    EXPECT_EQ(m_streamer->GetStats().staleCompletionsDropped, 1u);
    EXPECT_EQ(m_channel->uploads, 0);
}

TEST_F(AssetStreamingTest, RerequestRightAfterCancelIgnoresTheOldAttempt)
{
    auto first = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    first.Release();

    const auto second = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    ASSERT_EQ(m_jobs.size(), 2u);

    RunJob(0);
    m_streamer->Pump();
    EXPECT_EQ(StateOf(second), AssetLoadState::Decoding) << "旧 attempt の結果を採用してはいけない";

    RunAllJobs();
    m_streamer->Pump();
    ASSERT_NE(m_streamer->TryGet(second.Handle()), nullptr);
    EXPECT_EQ(m_streamer->TryGet(second.Handle())->value, "A#2");
    EXPECT_NE(first.Handle(), second.Handle()) << "返したスロットの世代は進む";
}

TEST_F(AssetStreamingTest, ProjectSwitchDropsInFlightResults)
{
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();

    m_streamer->ResetForProjectSwitch();
    m_streamer->RegisterChannel(m_channel);
    RunAllJobs();
    m_streamer->Pump();

    EXPECT_EQ(m_streamer->GetStatus(lease.Handle()).state, AssetLoadState::None);
    EXPECT_EQ(m_streamer->GetStats().staleCompletionsDropped, 1u);
    EXPECT_EQ(m_streamer->GetStats().jobsInFlight, 0u);
}

TEST_F(AssetStreamingTest, StoreClearDoesNotReviveOldHandles)
{
    auto& store = asset::AssetStore<StreamTestAsset>::Get();
    const auto before = store.Alloc(std::make_unique<StreamTestAsset>());
    store.Clear();
    const auto after = store.Alloc(std::make_unique<StreamTestAsset>());

    EXPECT_EQ(before.id, after.id);
    EXPECT_NE(before.gen, after.gen);
    EXPECT_FALSE(store.IsLive(before));
}

/// @name 失敗と再試行

TEST_F(AssetStreamingTest, FailureIsKeptUntilAnExplicitRetry)
{
    m_channel->scripts["A"].fail = true;
    auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    ASSERT_EQ(StateOf(lease), AssetLoadState::Failed);
    EXPECT_EQ(m_streamer->GetStatus(lease.Handle()).error, AssetLoadError::DecodeFailed);

    const auto again = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    EXPECT_TRUE(m_jobs.empty()) << "再要求だけでは投入し直さない (失敗ループの防止)";

    m_channel->scripts["A"].fail = false;
    EXPECT_TRUE(m_streamer->Retry(lease.Handle()));
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    EXPECT_EQ(StateOf(again), AssetLoadState::Ready);
}

/// @name 読み直し (ホットリロード)

TEST_F(AssetStreamingTest, ReloadsCompletingInReverseOrderKeepTheNewest)
{
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    ASSERT_EQ(m_streamer->TryGet(lease.Handle())->value, "A#1");

    ASSERT_TRUE(m_streamer->Reload(lease.Handle()));
    m_streamer->Pump();
    ASSERT_TRUE(m_streamer->Reload(lease.Handle()));
    m_streamer->Pump();
    ASSERT_EQ(m_jobs.size(), 2u);

    RunJob(1);
    m_streamer->Pump();
    EXPECT_EQ(m_streamer->TryGet(lease.Handle())->value, "A#3");

    RunJob(0);
    m_streamer->Pump();
    EXPECT_EQ(m_streamer->TryGet(lease.Handle())->value, "A#3") << "古い候補で上書きしてはいけない";
    EXPECT_EQ(m_streamer->GetStatus(lease.Handle()).publishedRevision, 3u);
}

TEST_F(AssetStreamingTest, FailedReloadKeepsThePublishedContent)
{
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();

    m_channel->scripts["A"].fail = true;
    ASSERT_TRUE(m_streamer->Reload(lease.Handle()));
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();

    const auto status = m_streamer->GetStatus(lease.Handle());
    EXPECT_EQ(status.state, AssetLoadState::Ready);
    EXPECT_TRUE(status.updateFailed);
    ASSERT_NE(m_streamer->TryGet(lease.Handle()), nullptr);
    EXPECT_EQ(m_streamer->TryGet(lease.Handle())->value, "A#1");
}

/// @name 依存

TEST_F(AssetStreamingTest, ParentWaitsForRequiredDependencyThenPublishes)
{
    m_channel->scripts["Parent"].dependencies = { { std::string(asset::AssetStreamTypeName<StreamTestAsset>()), "Child", true } };
    const auto parent = m_streamer->Request<StreamTestAsset>("Parent");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    EXPECT_EQ(StateOf(parent), AssetLoadState::WaitingDependencies);

    RunAllJobs();
    m_streamer->Pump();
    EXPECT_EQ(StateOf(parent), AssetLoadState::Ready);
}

TEST_F(AssetStreamingTest, RequiredDependencyFailureFailsTheParent)
{
    const std::string type(asset::AssetStreamTypeName<StreamTestAsset>());
    m_channel->scripts["Parent"].dependencies = { { type, "Child", true } };
    m_channel->scripts["Child"].fail = true;
    const auto parent = m_streamer->Request<StreamTestAsset>("Parent");
    for (int i = 0; i < 3; ++i) { m_streamer->Pump(); RunAllJobs(); }
    m_streamer->Pump();

    EXPECT_EQ(StateOf(parent), AssetLoadState::Failed);
    EXPECT_EQ(m_streamer->GetStatus(parent.Handle()).error, AssetLoadError::DependencyFailed);
}

TEST_F(AssetStreamingTest, OptionalDependencyFailureDoesNotBlockTheParent)
{
    const std::string type(asset::AssetStreamTypeName<StreamTestAsset>());
    m_channel->scripts["Parent"].dependencies = { { type, "Child", false } };
    m_channel->scripts["Child"].fail = true;
    const auto parent = m_streamer->Request<StreamTestAsset>("Parent");
    for (int i = 0; i < 3; ++i) { m_streamer->Pump(); RunAllJobs(); }
    m_streamer->Pump();

    EXPECT_EQ(StateOf(parent), AssetLoadState::Ready);
}

TEST_F(AssetStreamingTest, RequiredDependencyCycleFailsWithoutDeadlock)
{
    const std::string type(asset::AssetStreamTypeName<StreamTestAsset>());
    m_channel->scripts["A"].dependencies = { { type, "B", true } };
    m_channel->scripts["B"].dependencies = { { type, "A", true } };
    const auto a = m_streamer->Request<StreamTestAsset>("A");
    for (int i = 0; i < 4; ++i) { m_streamer->Pump(); RunAllJobs(); }
    m_streamer->Pump();

    EXPECT_EQ(StateOf(a), AssetLoadState::Failed);
    const auto b = m_streamer->Request<StreamTestAsset>("B");
    EXPECT_EQ(m_streamer->GetStatus(b.Handle()).error, AssetLoadError::DependencyCycle);
    EXPECT_NE(m_streamer->GetStatus(b.Handle()).message.find("A"), std::string::npos) << "経路を名指しする";
}

/// @name GPU 転送

TEST_F(AssetStreamingTest, PublishesOnlyAfterTheUploadCompletes)
{
    m_channel->asyncUpload = true;
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    EXPECT_EQ(StateOf(lease), AssetLoadState::Uploading);

    m_streamer->Pump();
    EXPECT_EQ(StateOf(lease), AssetLoadState::Uploading) << "フェンス未通過なら待たずに旧状態のまま";

    m_channel->completedTokens.insert(1);
    m_streamer->Pump();
    EXPECT_EQ(StateOf(lease), AssetLoadState::Ready);
}

TEST_F(AssetStreamingTest, DeviceResetDuringUploadReuploadsFromCpuData)
{
    m_channel->asyncUpload = true;
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    ASSERT_EQ(m_channel->uploads, 1);

    ++m_channel->deviceEpoch;
    m_streamer->Pump();
    m_streamer->Pump();

    EXPECT_EQ(m_channel->discardsAfterDeviceLoss, 1);
    EXPECT_EQ(m_channel->uploads, 2) << "デコードし直さずに転送だけやり直す";
    EXPECT_EQ(m_channel->snapshots, 1);
    m_channel->completedTokens.insert(2);
    m_streamer->Pump();
    EXPECT_EQ(StateOf(lease), AssetLoadState::Ready);
}

TEST_F(AssetStreamingTest, CancelDuringUploadDiscardsTheCandidate)
{
    m_channel->asyncUpload = true;
    auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    ASSERT_EQ(StateOf(lease), AssetLoadState::Uploading);

    lease.Release();
    EXPECT_EQ(m_channel->discards, 1);
    EXPECT_EQ(m_channel->discardsAfterDeviceLoss, 0);
}

TEST_F(AssetStreamingTest, UploadBytesPerPumpAreBoundedButOneLargeItemStillProceeds)
{
    AssetStreamer::Config config;
    config.maxUploadBytesPerPump = 1;
    CreateStreamer(config);

    const auto a = m_streamer->Request<StreamTestAsset>("A");
    const auto b = m_streamer->Request<StreamTestAsset>("B");
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();

    EXPECT_EQ(m_channel->uploads, 1) << "上限を超える 1 件目は通し、2 件目は次の Pump へ回す";
    m_streamer->Pump();
    EXPECT_EQ(m_channel->uploads, 2);
    EXPECT_EQ(StateOf(a), AssetLoadState::Ready);
    EXPECT_EQ(StateOf(b), AssetLoadState::Ready);
}

/// @name 同期 API との共存

TEST_F(AssetStreamingTest, SyncFillWhileLoadingDropsTheAsyncResult)
{
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();

    auto& store = asset::AssetStore<StreamTestAsset>::Get();
    ASSERT_TRUE(store.IsPending(store.cache.at("A")));
    auto filled = std::make_unique<StreamTestAsset>();
    filled->value = "sync";
    ASSERT_TRUE(store.Replace(store.cache.at("A"), std::move(filled)));
    m_streamer->NotifySyncFilled(asset::AssetStreamTypeName<StreamTestAsset>(), "A");

    RunAllJobs();
    m_streamer->Pump();

    EXPECT_EQ(StateOf(lease), AssetLoadState::Ready);
    EXPECT_EQ(m_streamer->TryGet(lease.Handle())->value, "sync");
    EXPECT_EQ(m_streamer->GetStats().syncMisses, 1u);
    EXPECT_EQ(m_streamer->GetStats().staleCompletionsDropped, 1u);
}

TEST_F(AssetStreamingTest, RequestForAnAlreadyLoadedAssetIsReadyImmediately)
{
    auto& store = asset::AssetStore<StreamTestAsset>::Get();
    auto loaded = std::make_unique<StreamTestAsset>();
    loaded->value = "loaded";
    store.cache["A"] = store.Alloc(std::move(loaded));

    const auto lease = m_streamer->Request<StreamTestAsset>("A");

    EXPECT_EQ(lease.Handle(), store.cache.at("A"));
    EXPECT_EQ(StateOf(lease), AssetLoadState::Ready);
    m_streamer->Pump();
    EXPECT_TRUE(m_jobs.empty());
}

TEST_F(AssetStreamingTest, CompleteNowPublishesSynchronouslyAndDropsTheAsyncResult)
{
    const auto lease = m_streamer->Request<StreamTestAsset>("A");
    m_streamer->Pump();

    ASSERT_TRUE(m_streamer->CompleteNow(lease.Handle()));
    EXPECT_EQ(StateOf(lease), AssetLoadState::Ready);
    EXPECT_EQ(m_streamer->TryGet(lease.Handle())->value, "now");

    RunAllJobs();
    m_streamer->Pump();
    EXPECT_EQ(m_streamer->TryGet(lease.Handle())->value, "now");
    EXPECT_EQ(m_streamer->GetStats().syncLoads, 1u);
    EXPECT_EQ(m_streamer->GetStats().staleCompletionsDropped, 1u);
}

/// @name 常駐と解放

TEST_F(AssetStreamingTest, UnusedAssetStaysResidentWithoutGraceOrBudget)
{
    auto lease = LoadReady("A");
    const auto handle = lease.Handle();
    lease.Release();
    PumpTimes(50);

    EXPECT_EQ(m_streamer->GetStatus(handle).state, AssetLoadState::Ready) << "既定では解放しない";
}

TEST_F(AssetStreamingTest, UnusedAssetIsEvictedAfterTheGracePeriod)
{
    AssetStreamer::Config config;
    config.evictionGracePumps = 3;
    CreateStreamer(config);

    auto lease = LoadReady("A");
    const auto handle = lease.Handle();
    lease.Release();
    PumpTimes(2);
    EXPECT_EQ(m_streamer->GetStatus(handle).state, AssetLoadState::Ready) << "猶予の間は残す";

    PumpTimes(2);
    EXPECT_EQ(m_streamer->GetStatus(handle).state, AssetLoadState::None);
    EXPECT_FALSE(asset::AssetStore<StreamTestAsset>::Get().IsLive(handle)) << "スロットを返して世代を進める";
    EXPECT_EQ(m_streamer->GetStats().evictions, 1u);
}

TEST_F(AssetStreamingTest, LeasedAssetIsNeverEvicted)
{
    AssetStreamer::Config config;
    config.evictionGracePumps = 1;
    config.residentBudgetBytes = 1;
    CreateStreamer(config);

    const auto lease = LoadReady("A");
    PumpTimes(5);

    EXPECT_EQ(StateOf(lease), AssetLoadState::Ready);
    EXPECT_TRUE(m_streamer->GetStats().overBudget) << "固定物だけで溢れていることを報告する";
}

TEST_F(AssetStreamingTest, OverBudgetEvictsTheLeastRecentlyReleasedFirst)
{
    AssetStreamer::Config config;
    config.residentBudgetBytes = 25;
    CreateStreamer(config);

    auto a = LoadReady("A");
    auto b = LoadReady("B");
    const auto c = LoadReady("C");
    const auto handleA = a.Handle();
    const auto handleB = b.Handle();
    a.Release();
    m_streamer->Pump();
    b.Release();
    m_streamer->Pump();

    EXPECT_EQ(m_streamer->GetStatus(handleA).state, AssetLoadState::None) << "先に手放された方から解放する";
    EXPECT_EQ(m_streamer->GetStatus(handleB).state, AssetLoadState::Ready) << "予算内に戻ったら止める";
    EXPECT_EQ(m_streamer->GetStats().residentBytes, 20u);
    EXPECT_FALSE(m_streamer->GetStats().overBudget);
}

TEST_F(AssetStreamingTest, SyncPinnedAssetIsNotEvicted)
{
    AssetStreamer::Config config;
    config.evictionGracePumps = 1;
    CreateStreamer(config);

    auto lease = LoadReady("A");
    const auto handle = lease.Handle();
    asset::AssetStore<StreamTestAsset>::Get().Pin(handle);
    lease.Release();
    PumpTimes(5);

    EXPECT_EQ(m_streamer->GetStatus(handle).state, AssetLoadState::Ready) << "同期 Load が固定したものは外さない";
    EXPECT_EQ(m_channel->evictions, 0);
}

TEST_F(AssetStreamingTest, OverBudgetDefersPrefetchAndRejectsNewKeys)
{
    AssetStreamer::Config config;
    config.residentBudgetBytes = 5;
    config.maxJobsInFlight = 1;
    CreateStreamer(config);

    asset::AssetRequestOptions required;
    required.priority = asset::AssetPriority::Required;
    asset::AssetRequestOptions prefetch;
    prefetch.priority = asset::AssetPriority::Prefetch;
    const auto a = m_streamer->Request<StreamTestAsset>("A", required);
    const auto b = m_streamer->Request<StreamTestAsset>("B", prefetch);
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();
    ASSERT_EQ(StateOf(a), AssetLoadState::Ready);

    EXPECT_TRUE(m_jobs.empty()) << "予算超過中は先読みを投入しない";
    EXPECT_EQ(StateOf(b), AssetLoadState::Queued);
    EXPECT_EQ(m_streamer->GetStats().deferredPrefetches, 1u);

    const auto c = m_streamer->Request<StreamTestAsset>("C");
    EXPECT_EQ(c.RequestError(), AssetLoadError::BudgetExceeded);
}

/// @name 品質段

TEST_F(AssetStreamingTest, DesiredQualityIsAppliedOnlyAfterTheHoldPeriod)
{
    m_channel->qualitySupported = true;
    AssetStreamer::Config config;
    config.qualityHoldPumps = 3;
    CreateStreamer(config);

    auto lease = LoadReady("A");
    const auto handle = lease.Handle();
    ASSERT_EQ(m_streamer->TryGet(handle)->quality, 0);

    lease.SetDesiredQuality(2);
    PumpTimes(2);
    EXPECT_EQ(m_streamer->GetStatus(handle).residentQuality, 0) << "ヒステリシスの間は動かさない";
    EXPECT_EQ(m_streamer->GetStatus(handle).desiredQuality, 2);

    PumpTimes(3);
    m_streamer->Pump();
    const auto status = m_streamer->GetStatus(handle);
    EXPECT_EQ(status.residentQuality, 2);
    EXPECT_EQ(m_streamer->TryGet(handle)->quality, 2) << "ハンドルはそのままで中身だけ差し替わる";
    EXPECT_EQ(status.residentBytes, 2u);
}

TEST_F(AssetStreamingTest, QualityAggregatesToTheBestAmongLeases)
{
    m_channel->qualitySupported = true;
    asset::AssetRequestOptions low;
    low.quality = 3;
    const auto coarse = m_streamer->Request<StreamTestAsset>("A", low);
    asset::AssetRequestOptions high;
    high.quality = 1;
    const auto fine = m_streamer->Request<StreamTestAsset>("A", high);
    m_streamer->Pump();
    RunAllJobs();
    m_streamer->Pump();

    EXPECT_EQ(m_streamer->TryGet(fine.Handle())->quality, 1) << "初回ロードから最も高い希望品質で作る";
}

TEST_F(AssetStreamingTest, PersistentOverBudgetLowersQualityOfLeasedAssets)
{
    m_channel->qualitySupported = true;
    AssetStreamer::Config config;
    config.residentBudgetBytes = 8;
    config.qualityHoldPumps = 1;
    CreateStreamer(config);

    const auto lease = LoadReady("A");
    ASSERT_TRUE(m_streamer->GetStats().overBudget);
    PumpTimes(6);
    m_streamer->Pump();

    EXPECT_GE(m_streamer->GetStats().qualityBias, 1);
    EXPECT_GE(m_streamer->GetStatus(lease.Handle()).residentQuality, 1) << "任意の高品質表現を落として空きを作る";
    EXPECT_FALSE(m_streamer->GetStats().overBudget);
}

} // namespace fbzz::tests
