/// @file    SchedulerBatchingTests.cpp
/// @brief   SystemScheduler が競合と順序宣言からバッチ列を組み、固定ステップを刻むことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// Build() が組むバッチ列は «どの System が同時に走ってよいか» そのもの。競合を見落とせば
/// 2 本の System が同じ Component を同時に書き、再現しないデータ競合になる。逆に競合を
/// 過剰に見れば全部が直列化して並列化の意味が消える。どちらも実行結果が «だいたい合う»
/// ため、症状からは辿れない。
///
/// 固定ステップの刻み方も同じ性質を持つ。追いつき上限が効かないと、ブレークポイントで
/// 止めた次のフレームに数百回ぶんの物理が一気に走って世界が吹き飛ぶ。
///
/// 宣言そのものの値契約は `SchedulerDeclarationTests`、Build/Find/Shutdown の基本は
/// `SchedulerExecutionTests` が見る。ここは «組み上がった順序» だけを見る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/LogSinks.hpp>

#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Core/Scheduler/SystemScheduler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Physics/World.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::tests {
namespace {

/// 実行順の記録先。同じバッチに入った System はワーカースレッドから同時に書くため、
/// 記録側で直列化しないとテスト自身がデータ競合になる。
class Trace {
public:
    void Record(std::string_view name)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.emplace_back(name);
    }

    [[nodiscard]] std::vector<std::string> Entries() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries;
    }

    void Clear()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

private:
    mutable std::mutex       m_mutex;
    std::vector<std::string> m_entries;
};

/// System は AddSystem<T>() から default 構築されるので、記録先をコンストラクタで
/// 渡せない。ファイル内で 1 つだけ持ち、fixture が各テストの先頭で空にする。
Trace& SharedTrace()
{
    static Trace trace;
    return trace;
}

struct Position {};
struct Velocity {};

/// 名前を記録するだけの System。順序制約は «型» で宣言するため、
/// テスト用の System は中身が同じでも別の型として書く必要がある。
struct WriteA final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "WriteA"; }
    Phase            GetPhase() const override { return Phase::Script; }
    ComponentAccess  GetAccess() const override { return ComponentAccess{}.Writes<Position>(); }
};

struct WriteB final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "WriteB"; }
    Phase            GetPhase() const override { return Phase::Script; }
    ComponentAccess  GetAccess() const override { return ComponentAccess{}.Writes<Position>(); }
};

struct ReadPosition final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "ReadPosition"; }
    Phase            GetPhase() const override { return Phase::Script; }
    ComponentAccess  GetAccess() const override { return ComponentAccess{}.Reads<Position>(); }
};

struct ReadVelocity final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "ReadVelocity"; }
    Phase            GetPhase() const override { return Phase::Script; }
    ComponentAccess  GetAccess() const override { return ComponentAccess{}.Reads<Velocity>(); }
};

struct TouchesEverything final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "TouchesEverything"; }
    Phase            GetPhase() const override { return Phase::Script; }
    ComponentAccess  GetAccess() const override { return ComponentAccess{}.Unrestricted(); }
};

struct Leader final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "Leader"; }
    Phase            GetPhase() const override { return Phase::Script; }
};

struct FollowerAfterLeader final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "FollowerAfterLeader"; }
    Phase            GetPhase() const override { return Phase::Script; }
    OrderingHints    GetOrder() const override { return OrderingHints{}.After<Leader>(); }
};

struct Closer final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "Closer"; }
    Phase            GetPhase() const override { return Phase::Script; }
};

struct OpenerBeforeCloser final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "OpenerBeforeCloser"; }
    Phase            GetPhase() const override { return Phase::Script; }
    OrderingHints    GetOrder() const override { return OrderingHints{}.Before<Closer>(); }
};

struct FixedStepCounter final : ISystem {
    void Update(SystemContext&) override { SharedTrace().Record(Name()); }
    std::string_view Name()     const override { return "FixedStepCounter"; }
    Phase            GetPhase() const override { return Phase::Physics; }
};

struct LateOnly final : ISystem {
    void Update(SystemContext& ctx) override
    {
        SharedTrace().Record(Name());
        observedAlpha = ctx.interpolationAlpha;
    }
    std::string_view Name()     const override { return "LateOnly"; }
    Phase            GetPhase() const override { return Phase::LateUpdate; }

    float observedAlpha = -1.0f;
};

} // namespace

class SchedulerBatchingTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        // 同じバッチの System はワーカースレッドで走る。プールが無いと Submit が落ちる。
        TaskSystem::Init(2);
        SharedTrace().Clear();
    }

    void TearDown() override
    {
        scheduler.Shutdown();
        TaskSystem::Shutdown();
        EngineFixture::TearDown();
    }

    SystemContext Context(float dt) { return SystemContext{ scene, world, nullptr, nullptr,
                                                            dt, dt, true }; }

    [[nodiscard]] std::vector<std::string> Trail() const { return SharedTrace().Entries(); }

    [[nodiscard]] std::size_t CountOf(std::string_view name) const
    {
        const std::vector<std::string> entries = SharedTrace().Entries();
        return static_cast<std::size_t>(
            std::count(entries.begin(), entries.end(), std::string(name)));
    }

    SystemScheduler      scheduler;
    scene::Scene         scene;
    physics::World       world;
};

// --- 競合からの直列化 -------------------------------------------------------

TEST_F(SchedulerBatchingTest, SerialisesSystemsThatWriteTheSameComponent)
{
    scheduler.AddSystem<WriteA>();
    scheduler.AddSystem<WriteB>();
    scheduler.Build();

    scheduler.Update(Context(testkit::kFixedDeltaTime));

    // 同じ Component を書く 2 本を同時に走らせると、再現しない壊れ方をする。
    // 順序の宣言が無い競合は «登録順» で決めるのが約束。
    EXPECT_EQ(Trail(), (std::vector<std::string>{ "WriteA", "WriteB" }));
}

TEST_F(SchedulerBatchingTest, SerialisesAnUnrestrictedSystemAgainstEveryOther)
{
    scheduler.AddSystem<TouchesEverything>();
    scheduler.AddSystem<ReadPosition>();
    scheduler.Build();

    scheduler.Update(Context(testkit::kFixedDeltaTime));

    // 「何を触るか宣言できない」System は、全部と競合する扱いにするしかない。
    EXPECT_EQ(Trail(), (std::vector<std::string>{ "TouchesEverything", "ReadPosition" }));
}

TEST_F(SchedulerBatchingTest, RunsSystemsWithDisjointAccessTogether)
{
    scheduler.AddSystem<ReadPosition>();
    scheduler.AddSystem<ReadVelocity>();
    scheduler.Build();

    scheduler.Update(Context(testkit::kFixedDeltaTime));

    // 触るものが重ならない 2 本は同じバッチに入り、並列に走る。順序は保証されないので
    // «両方走ったこと» だけを見る。ここが直列化していると並列化の意味が消える。
    EXPECT_EQ(CountOf("ReadPosition"), 1u);
    EXPECT_EQ(CountOf("ReadVelocity"), 1u);
}

// --- 順序宣言 ---------------------------------------------------------------

TEST_F(SchedulerBatchingTest, HonoursAnAfterHintEvenAgainstRegistrationOrder)
{
    scheduler.AddSystem<FollowerAfterLeader>();
    scheduler.AddSystem<Leader>();
    scheduler.Build();

    scheduler.Update(Context(testkit::kFixedDeltaTime));

    // 登録順に負けると、宣言した依存が «たまたま順番が合っていただけ» になる。
    EXPECT_EQ(Trail(), (std::vector<std::string>{ "Leader", "FollowerAfterLeader" }));
}

TEST_F(SchedulerBatchingTest, HonoursABeforeHintEvenAgainstRegistrationOrder)
{
    scheduler.AddSystem<Closer>();
    scheduler.AddSystem<OpenerBeforeCloser>();
    scheduler.Build();

    scheduler.Update(Context(testkit::kFixedDeltaTime));

    EXPECT_EQ(Trail(), (std::vector<std::string>{ "OpenerBeforeCloser", "Closer" }));
}

// --- 固定ステップ -----------------------------------------------------------

TEST_F(SchedulerBatchingTest, RunsAFixedStepPhaseOncePerElapsedInterval)
{
    scheduler.ConfigurePhase(Phase::Physics, PhaseConfig{ true, 60, 8.0f });
    scheduler.AddSystem<FixedStepCounter>();
    scheduler.Build();

    scheduler.Update(Context(3.5f / 60.0f));

    // 3.5 ステップぶんの時間なら 3 回。端数は次フレームへ持ち越す。
    EXPECT_EQ(CountOf("FixedStepCounter"), 3u);
}

TEST_F(SchedulerBatchingTest, ClampsTheFixedStepCatchUpAfterALongStall)
{
    scheduler.ConfigurePhase(Phase::Physics, PhaseConfig{ true, 60, 3.5f });
    scheduler.AddSystem<FixedStepCounter>();
    scheduler.Build();

    scheduler.Update(Context(10.0f));

    // 上限が無ければ 600 回。ブレークポイントで止めた次のフレームに 10 秒ぶんの
    // 物理が一気に走ると、掴んでいた物が視界の外まで飛ぶ。
    EXPECT_EQ(CountOf("FixedStepCounter"), 3u);
}

TEST_F(SchedulerBatchingTest, RunsExactlyOneFixedStepWhileSingleStepping)
{
    scheduler.ConfigurePhase(Phase::Physics, PhaseConfig{ true, 60, 8.0f });
    scheduler.AddSystem<FixedStepCounter>();
    scheduler.Build();

    scheduler.SetSingleStep(true);
    scheduler.Update(Context(0.0f));
    scheduler.Update(Context(0.0f));

    // «1 コマ進める» は 1 回きり。解除し忘れると以降ずっと dt 無視で回り続ける。
    EXPECT_EQ(CountOf("FixedStepCounter"), 1u);
}

TEST_F(SchedulerBatchingTest, ResetAccumulatorDropsTheLeftoverFraction)
{
    scheduler.ConfigurePhase(Phase::Physics, PhaseConfig{ true, 60, 8.0f });
    scheduler.AddSystem<FixedStepCounter>();
    scheduler.Build();

    scheduler.Update(Context(1.5f / 60.0f));   // 1 回実行し、0.5 ステップぶん残る
    scheduler.ResetAccumulator();
    scheduler.Update(Context(0.6f / 60.0f));   // 残りを捨てていれば 1 ステップに届かない

    // Play を押し直したときに前セッションの端数が残っていると、開始直後に
    // 余分な 1 ステップが走って «押した瞬間に少し動く»。
    EXPECT_EQ(CountOf("FixedStepCounter"), 1u);
}

// --- LateUpdate -------------------------------------------------------------

TEST_F(SchedulerBatchingTest, UpdateSkipsTheLateUpdatePhase)
{
    scheduler.AddSystem<Leader>();
    scheduler.AddSystem<LateOnly>();
    scheduler.Build();

    scheduler.Update(Context(testkit::kFixedDeltaTime));

    EXPECT_EQ(CountOf("Leader"), 1u);
    EXPECT_EQ(CountOf("LateOnly"), 0u);
}

TEST_F(SchedulerBatchingTest, LateUpdateRunsOnlyTheLateUpdatePhase)
{
    scheduler.AddSystem<Leader>();
    scheduler.AddSystem<LateOnly>();
    scheduler.Build();

    SystemContext context = Context(testkit::kFixedDeltaTime);
    scheduler.LateUpdate(context);

    EXPECT_EQ(CountOf("LateOnly"), 1u);
    EXPECT_EQ(CountOf("Leader"), 0u);
}

TEST_F(SchedulerBatchingTest, LateUpdateReportsHowFarTheFixedStepHasProgressed)
{
    scheduler.ConfigurePhase(Phase::Physics, PhaseConfig{ true, 60, 8.0f });
    scheduler.AddSystem<LateOnly>();
    scheduler.Build();

    auto* lateSystem = static_cast<LateOnly*>(scheduler.FindSystem("LateOnly"));
    ASSERT_NE(lateSystem, nullptr);

    scheduler.Update(Context(1.5f / 60.0f));   // 1 ステップ消化して 0.5 ステップ残る
    SystemContext context = Context(1.5f / 60.0f);
    scheduler.LateUpdate(context);

    // 描画はこの係数で «前回の確定姿勢と次の姿勢» を補間する。0 に固定すると
    // 60Hz 固定の物理が 144Hz の画面でカクつく。
    EXPECT_NEAR(lateSystem->observedAlpha, 0.5f, testkit::kLooseTolerance);
}

// --- デバッグ出力 -----------------------------------------------------------

TEST_F(SchedulerBatchingTest, DumpGraphNamesEveryScheduledSystem)
{
    // EngineFixture は既定でログを ERROR まで絞る。DumpGraph は INFO で出るので開ける。
    SetLogLevel(core::LogLevel::INFO);
    testkit::RecordingLogSink sink;
    const testkit::ScopedLogSink scoped(&sink);
    scheduler.AddSystem<WriteA>();
    scheduler.AddSystem<WriteB>();
    scheduler.Build();

    scheduler.DumpGraph();

    // «どの System がどのバッチに入ったか» を読む唯一の窓口。ここが欠けると、
    // 並列化されていない理由を調べる手段が無くなる。
    ASSERT_NE(sink.Find("WriteA"), nullptr);
    EXPECT_NE(sink.Find("WriteB"), nullptr);
}

} // namespace fbzz::tests
