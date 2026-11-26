/// @file    SystemScheduler.cpp
/// @brief   Phase × DAG によるシステム実行管理。
/// @author  Hasegawa Jin
/// @date    2026-06-18

/// @note Build() でトポロジカルソートし、競合しない System ペアを同バッチに配置する。
/// @note 同バッチの System は TaskSystem 経由で並列実行される。
#include "Engine/Core/Scheduler/SystemScheduler.hpp"
#include "Engine/Core/Concurrency/TaskSystem.hpp"
#include "Engine/Profiler/ProfileScope.hpp"
#include "Engine/Profiler/Profiler.hpp"
#include "Engine/Core/Diagnostics/Logger.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <future>
#include <queue>
#include <sstream>
#include <typeindex>

namespace fbzz {
namespace {

using SystemBatch = std::vector<ISystem*>;
using SystemBatches = std::vector<SystemBatch>;

bool HasConflict(const ComponentAccess& a, const ComponentAccess& b)
{
    if (a.unrestricted || b.unrestricted) {
        return true;
    }

    const auto intersects = [](const std::vector<std::type_index>& left,
                               const std::vector<std::type_index>& right) {
        for (const auto& type : left) {
            if (std::find(right.begin(), right.end(), type) != right.end()) {
                return true;
            }
        }
        return false;
    };

    return intersects(a.writes, b.writes) || intersects(a.writes, b.reads) || intersects(b.writes, a.reads);
}

bool HasPath(const std::vector<std::vector<size_t>>& successors, size_t from, size_t to)
{
    std::vector<bool> visited(successors.size(), false);
    std::vector<size_t> pending{from};
    while (!pending.empty()) {
        const size_t current = pending.back();
        pending.pop_back();
        if (current == to) {
            return true;
        }
        if (visited[current]) {
            continue;
        }
        visited[current] = true;
        for (size_t next : successors[current]) {
            pending.push_back(next);
        }
    }
    return false;
}

size_t BuildPhaseBatches(const SystemBatch& systems, SystemBatches& batches)
{
    const size_t systemCount = systems.size();
    std::vector<std::vector<size_t>> successors(systemCount);
    std::vector<int> dependenciesRemaining(systemCount, 0);
    const auto addDependency = [&](size_t first, size_t next) {
        successors[first].push_back(next);
        ++dependenciesRemaining[next];
    };

    // 明示の順序を先に登録する。同じ Phase 内の同型 System はすべてヒントの対象になる。
    for (size_t index = 0; index < systemCount; ++index) {
        const OrderingHints hints = systems[index]->GetOrder();
        for (const auto& type : hints.after) {
            for (size_t candidate = 0; candidate < systemCount; ++candidate) {
                if (std::type_index(typeid(*systems[candidate])) == type) {
                    addDependency(candidate, index);
                }
            }
        }
        for (const auto& type : hints.before) {
            for (size_t candidate = 0; candidate < systemCount; ++candidate) {
                if (std::type_index(typeid(*systems[candidate])) == type) {
                    addDependency(index, candidate);
                }
            }
        }
    }

    // 両方向に経路がない競合だけを登録順で直列化する。間接的な依存にも従うことで逆向きの辺による循環を防ぐ。
    for (size_t first = 0; first < systemCount; ++first) {
        for (size_t second = first + 1; second < systemCount; ++second) {
            if (!HasConflict(systems[first]->GetAccess(), systems[second]->GetAccess())) {
                continue;
            }
            if (!HasPath(successors, first, second) && !HasPath(successors, second, first)) {
                addDependency(first, second);
            }
        }
    }

    std::queue<size_t> ready;
    for (size_t index = 0; index < systemCount; ++index) {
        if (dependenciesRemaining[index] == 0) {
            ready.push(index);
        }
    }

    size_t processed = 0;
    while (!ready.empty()) {
        // Kahn 法の各ラウンドの先頭でバッチ幅を確定する。今回の完了で実行可能になる後続は次のバッチへ回す。
        const size_t batchSize = ready.size();
        SystemBatch batch;
        for (size_t slot = 0; slot < batchSize; ++slot) {
            const size_t index = ready.front();
            ready.pop();
            batch.push_back(systems[index]);
            for (size_t next : successors[index]) {
                if (--dependenciesRemaining[next] == 0) {
                    ready.push(next);
                }
            }
            ++processed;
        }
        batches.push_back(std::move(batch));
    }
    return processed;
}

}

void SystemScheduler::ConfigurePhase(Phase phase, PhaseConfig cfg)
{
    m_phaseConfigs[static_cast<size_t>(phase)] = cfg;
}

void SystemScheduler::AddSystemPtr(std::unique_ptr<ISystem> sys)
{
    assert(!m_built && "AddSystem() must be called before Build()");
    m_systems.push_back(std::move(sys));
}

bool SystemScheduler::Build()
{
    assert(!m_built);
    if (m_built) {
        return false;
    }

    // Phase 間の順序は Update にあり、ここでは各 Phase 内の依存だけを実行バッチに変換する。
    for (size_t phaseIndex = 0; phaseIndex < static_cast<size_t>(Phase::Count); ++phaseIndex) {
        const Phase phase = static_cast<Phase>(phaseIndex);
        auto& batches = m_batches[phaseIndex];
        batches.clear();

        SystemBatch phaseSystems;
        for (auto& system : m_systems) {
            if (system->GetPhase() == phase) {
                phaseSystems.push_back(system.get());
            }
        }
        if (phaseSystems.empty()) {
            continue;
        }

        const size_t processed = BuildPhaseBatches(phaseSystems, batches);
        if (processed != phaseSystems.size()) {
            FBZZ_LOG_ERROR("SystemScheduler: cyclic ordering in phase %zu (%zu/%zu systems).",
                           phaseIndex, processed, phaseSystems.size());
            for (auto& phaseBatches : m_batches) {
                phaseBatches.clear();
            }
            return false;
        }
    }

    // 全 Phase を検証してから初期化する。循環のあるスケジュールで OnInit の副作用を一部だけ起こさない。
    for (auto& system : m_systems) {
        system->OnInit();
    }

    m_built = true;
    return true;
}

void SystemScheduler::SetSingleStep(bool enabled)
{
    m_singleStep = enabled;
}

void SystemScheduler::ResetAccumulator()
{
    m_accumulator = 0.0f;
}

namespace {

void RunParallelBatch(const SystemBatch& systems, SystemContext& context)
{
    std::vector<std::future<void>> tasks;
    std::vector<double> elapsedMs(systems.size(), 0.0);
    std::vector<double> queueWaitMs(systems.size(), 0.0);
    tasks.reserve(systems.size());
    for (size_t index = 0; index < systems.size(); ++index) {
        ISystem* system = systems[index];
        double* elapsed = &elapsedMs[index];
        double* queueWait = &queueWaitMs[index];
        const auto submittedAt = std::chrono::steady_clock::now();
        tasks.push_back(TaskSystem::Submit([system, &context, elapsed, queueWait, submittedAt] {
            const auto startedAt = std::chrono::steady_clock::now();
            *queueWait = std::chrono::duration<double, std::milli>(startedAt - submittedAt).count();
            system->Update(context);
            *elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - startedAt).count();
        }));
    }

    // 後続バッチはこのバッチの書き込みを読む。全タスクの完了と書き込みの可視化をここで待つ。
    const auto waitBegin = std::chrono::steady_clock::now();
    for (auto& task : tasks) {
        task.get();
    }
    const double mainWaitMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - waitBegin).count();

    // ワーカーで測った値は呼び出し側でまとめ、タスクの完了順に左右されないバッチ順で記録する。
    double totalQueueWaitMs = 0.0;
    for (size_t index = 0; index < systems.size(); ++index) {
        totalQueueWaitMs += queueWaitMs[index];
        profiler::Profiler::PushSample(
            profiler::ProfilerMarker(systems[index]->Name().data()), elapsedMs[index]);
    }
    profiler::Profiler::PushSample(profiler::ProfilerMarker("Scheduler queue wait"), totalQueueWaitMs);
    profiler::Profiler::PushSample(profiler::ProfilerMarker("Scheduler main wait"), mainWaitMs);
}

}

void SystemScheduler::RunPhase(Phase phase, SystemContext& ctx)
{
    assert(m_built);
    const size_t phaseIndex = static_cast<size_t>(phase);
    const PhaseConfig& configuration = m_phaseConfigs[phaseIndex];

    const auto runBatches = [&](SystemContext& context) {
        for (const auto& batch : m_batches[phaseIndex]) {
            // 前のバッチが状態を書き換え得るので、ShouldRun は各バッチの直前に評価する。
            SystemBatch runnable;
            for (ISystem* system : batch) {
                const RunMode mode = system->GetRunMode();
                if (mode == RunMode::SimOnly && !context.simulating) {
                    continue;
                }
                if (mode == RunMode::EditorOnly && context.simulating) {
                    continue;
                }
                // 実行間隔は外側のフレーム基準。1 フレーム内で追いつく固定ステップも同じ番号を使う。
                if (m_frameCounter % system->RunInterval() != 0) {
                    continue;
                }
                if (!system->ShouldRun(context)) {
                    continue;
                }
                runnable.push_back(system);
            }
            if (runnable.empty()) {
                continue;
            }

            // 実行対象が複数あり、ワーカーに並行実行を割り当てられるときだけ仕事を分ける。
            const bool dispatchInParallel = runnable.size() > 1 && TaskSystem::WorkerCount() > 1;
            if (dispatchInParallel) {
                RunParallelBatch(runnable, context);
                continue;
            }
            for (ISystem* system : runnable) {
                FBZZ_PROFILE_SCOPE(system->Name().data());
                system->Update(context);
            }
        }
    };

    if (!configuration.fixedStep) {
        runBatches(ctx);
        return;
    }

    const float fixedDt = 1.0f / static_cast<float>(configuration.hz);
    const float maxAccumulatedTime = fixedDt * configuration.maxCatchUp;
    SystemContext fixedCtx = ctx;
    fixedCtx.fixedDt = fixedDt;

    if (m_singleStep) {
        // 単発ステップでは固定刻みを一度だけ実行し、通常の蓄積時間を消費しない。
        runBatches(fixedCtx);
        m_singleStep = false;
        return;
    }

    // 長いフレームの後に固定ステップだけで次のフレームを占有しないよう、追いつく時間を制限する。
    m_accumulator = std::min(m_accumulator + ctx.dt, maxAccumulatedTime);
    while (m_accumulator >= fixedDt) {
        runBatches(fixedCtx);
        m_accumulator -= fixedDt;
    }
}

void SystemScheduler::Update(SystemContext ctx)
{
    assert(m_built);
    if (!m_built) {
        return;
    }
    RunPhase(Phase::PreScript,   ctx);
    RunPhase(Phase::Script,      ctx);

    // Script が書いたローカル姿勢と流れを物理の前に確定し、物理の書き戻しを Navigation が読む。
    RunPhase(Phase::PrePhysics,  ctx);
    RunPhase(Phase::Physics,     ctx);
    RunPhase(Phase::PostPhysics, ctx);
    RunPhase(Phase::Navigation,  ctx);
    RunPhase(Phase::LateScript,  ctx);

    // シミュレーションからの破棄要求を処理してから、別入口の LateUpdate が描画用の姿勢を組む。
    RunPhase(Phase::Cleanup,     ctx);
    ++m_frameCounter;
}

void SystemScheduler::LateUpdate(SystemContext ctx)
{
    assert(m_built);
    if (!m_built) {
        return;
    }
    const PhaseConfig& physicsConfig = m_phaseConfigs[static_cast<size_t>(Phase::Physics)];
    if (physicsConfig.fixedStep && physicsConfig.hz > 0)
    {
        const float fixedDt = 1.0f / static_cast<float>(physicsConfig.hz);
        // Update() 後に残った accumulator は、現在の確定姿勢から次の fixed step
        // までの経過割合。LateUpdate の描画だけをこの係数で補間する。
        ctx.interpolationAlpha = std::clamp(m_accumulator / fixedDt, 0.0f, 1.0f);
    }
    RunPhase(Phase::LateUpdate, ctx);
}

void SystemScheduler::Shutdown()
{
    if (m_built) {
        for (auto& sys : m_systems) {
            sys->OnShutdown();
        }
    }

    // 非所有参照を持つバッチは破棄済みシステムへ実行を渡さないように空にする。
    m_systems.clear();
    for (auto& b : m_batches) {
        b.clear();
    }
    m_built = false;
}

/// @name デバッグ

ISystem* SystemScheduler::FindSystem(std::string_view name) const
{
    for (const auto& system : m_systems) {
        if (system->Name() == name) {
            return system.get();
        }
    }
    return nullptr;
}

void SystemScheduler::DumpGraph() const
{
    static const char* phaseNames[] = {
        "PreScript", "Script", "PrePhysics", "Physics", "PostPhysics",
        "Navigation", "LateScript", "Cleanup", "LateUpdate"
    };
    std::ostringstream oss;
    oss << "=== SystemScheduler Graph ===\n";
    for (size_t pi = 0; pi < static_cast<size_t>(Phase::Count); ++pi) {
        const auto& batches = m_batches[pi];
        if (batches.empty()) {
            continue;
        }
        oss << "Phase::" << phaseNames[pi] << "\n";
        for (size_t bi = 0; bi < batches.size(); ++bi) {
            oss << "  Batch[" << bi << "]: ";
            for (size_t si = 0; si < batches[bi].size(); ++si) {
                if (si) {
                    oss << " || ";
                }
                oss << batches[bi][si]->Name();
            }
            oss << "\n";
        }
    }
    // Logger は printf 形式の可変長引数。"{}" は書式指定として解釈されずそのまま出力され、
    // std::string を ... へ渡すのは未定義動作になるため %s を使う。
    FBZZ_LOG_INFO("%s", oss.str().c_str());
}

}
