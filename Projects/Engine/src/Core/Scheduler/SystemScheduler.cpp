// FBZZ Engine
// SystemScheduler.cpp | fbzz
// Phase × DAG によるシステム実行管理。
// Build() でトポロジカルソートし、競合しない System ペアを同バッチに配置する。
// 同バッチの System は TaskSystem 経由で並列実行される。
#include "Engine/Core/Scheduler/SystemScheduler.hpp"
#include "Engine/Core/Concurrency/TaskSystem.hpp"
#include "Engine/Profiler/ProfileScope.hpp"
#include "Engine/Core/Logger.hpp"
#include <algorithm>
#include <cassert>
#include <future>
#include <queue>
#include <sstream>
#include <typeindex>
#include <unordered_map>

namespace fbzz {

// ─── 内部ユーティリティ ───────────────────────────────────────────────────────

static bool HasConflict(const ComponentAccess& a, const ComponentAccess& b)
{
    if (a.unrestricted || b.unrestricted) return true;

    auto intersects = [](const std::vector<std::type_index>& x,
                         const std::vector<std::type_index>& y) {
        for (const auto& xi : x)
            for (const auto& yi : y)
                if (xi == yi) return true;
        return false;
    };

    if (intersects(a.writes, b.writes)) return true;  // write-write
    if (intersects(a.writes, b.reads))  return true;  // a write, b read
    if (intersects(b.writes, a.reads))  return true;  // b write, a read
    return false;
}

// ─── Public API ──────────────────────────────────────────────────────────────

void SystemScheduler::ConfigurePhase(Phase phase, PhaseConfig cfg)
{
    m_phaseConfigs[static_cast<size_t>(phase)] = cfg;
}

void SystemScheduler::AddSystemPtr(std::unique_ptr<ISystem> sys)
{
    assert(!m_built && "AddSystem() must be called before Build()");
    m_systems.push_back(std::move(sys));
}

void SystemScheduler::Build()
{
    assert(!m_built);

    for (size_t pi = 0; pi < static_cast<size_t>(Phase::Count); ++pi) {
        const Phase phase = static_cast<Phase>(pi);
        auto& batches = m_batches[pi];
        batches.clear();

        // この Phase に属する System を収集
        std::vector<ISystem*> phSystems;
        for (auto& sys : m_systems)
            if (sys->GetPhase() == phase)
                phSystems.push_back(sys.get());

        if (phSystems.empty()) continue;

        const size_t n = phSystems.size();

        // ── Kahn's algorithm で DAG をトポロジカルソートしてバッチ列を作る ──
        // 隣接行列: adj[i] = i が先、j が後（i→j の依存）
        std::vector<std::vector<int>> adj(n);
        std::vector<int> inDegree(n, 0);

        auto indexOf = [&](const ISystem* p) -> int {
            for (size_t k = 0; k < n; ++k)
                if (phSystems[k] == p) return static_cast<int>(k);
            return -1;
        };

        // OrderingHints から辺を張る
        for (size_t i = 0; i < n; ++i) {
            const OrderingHints hints = phSystems[i]->GetOrder();
            for (const auto& tid : hints.after) {
                for (size_t j = 0; j < n; ++j) {
                    if (std::type_index(typeid(*phSystems[j])) == tid) {
                        // j が先に来る (j → i)
                        adj[j].push_back(static_cast<int>(i));
                        inDegree[i]++;
                    }
                }
            }
            for (const auto& tid : hints.before) {
                for (size_t j = 0; j < n; ++j) {
                    if (std::type_index(typeid(*phSystems[j])) == tid) {
                        // i が先に来る (i → j)
                        adj[i].push_back(static_cast<int>(j));
                        inDegree[j]++;
                    }
                }
            }
        }

        // ComponentAccess 競合から追加の辺を張る（順序は登録順で決定）
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                if (!HasConflict(phSystems[i]->GetAccess(), phSystems[j]->GetAccess())) continue;
                // 競合する場合、OrderingHints で既に辺がなければ登録順 (i→j) を使う
                bool alreadyOrdered = false;
                for (int v : adj[i]) if (v == static_cast<int>(j)) { alreadyOrdered = true; break; }
                for (int v : adj[j]) if (v == static_cast<int>(i)) { alreadyOrdered = true; break; }
                if (!alreadyOrdered) {
                    adj[i].push_back(static_cast<int>(j));
                    inDegree[j]++;
                }
            }
        }

        // Kahn's algorithm: 同一レベルのノードを 1 バッチにまとめる
        std::queue<int> q;
        for (size_t i = 0; i < n; ++i)
            if (inDegree[i] == 0) q.push(static_cast<int>(i));

        size_t processed = 0;
        while (!q.empty()) {
            std::vector<ISystem*> batch;
            // 現在キューにある全ノード = 同じレベル → 1 バッチ
            std::vector<int> levelNodes;
            while (!q.empty()) {
                levelNodes.push_back(q.front());
                q.pop();
            }
            for (int idx : levelNodes) {
                batch.push_back(phSystems[idx]);
                for (int next : adj[idx]) {
                    if (--inDegree[next] == 0) q.push(next);
                }
                ++processed;
            }
            batches.push_back(std::move(batch));
        }

        assert(processed == n && "Circular dependency detected in System ordering!");
    }

    for (auto& sys : m_systems)
        sys->OnInit();

    m_built = true;
}

void SystemScheduler::SetSingleStep(bool enabled)
{
    m_singleStep = enabled;
}

void SystemScheduler::ResetAccumulator()
{
    m_accumulator = 0.0f;
    m_physicsAlpha = 0.0f;
}

// ─── Frame Update ────────────────────────────────────────────────────────────

void SystemScheduler::RunPhase(Phase p, SystemContext& ctx)
{
    assert(m_built);
    const size_t pi = static_cast<size_t>(p);
    const PhaseConfig& cfg = m_phaseConfigs[pi];

    auto runBatches = [&](SystemContext& c) {
        for (auto& batch : m_batches[pi]) {
            // RunMode / ShouldRun フィルタリングしてから実行する System を決定
            std::vector<ISystem*> toRun;
            for (ISystem* sys : batch) {
                const RunMode mode = sys->GetRunMode();
                if (mode == RunMode::SimOnly    && !c.simulating) continue;
                if (mode == RunMode::EditorOnly &&  c.simulating) continue;
                if (m_frameCounter % sys->RunInterval() != 0)     continue;
                if (!sys->ShouldRun(c))                            continue;
                toRun.push_back(sys);
            }
            if (toRun.empty()) continue;

            if (toRun.size() == 1) {
                FBZZ_PROFILE_SCOPE(toRun[0]->Name().data());
                toRun[0]->Update(c);
            } else {
                std::vector<std::future<void>> futs;
                futs.reserve(toRun.size());
                for (ISystem* sys : toRun)
                    futs.push_back(TaskSystem::Submit([sys, &c]{ sys->Update(c); }));
                for (auto& f : futs) f.get();
            }
        }
    };

    if (!cfg.fixedStep) {
        runBatches(ctx);
        return;
    }

    // 固定ステップループ（PhysicsSystem 用）
    const float fixedDt  = 1.0f / static_cast<float>(cfg.hz);
    const float maxAccum = fixedDt * cfg.maxCatchUp;
    SystemContext fixedCtx = ctx;
    fixedCtx.fixedDt = fixedDt;

    if (m_singleStep) {
        runBatches(fixedCtx);
        m_singleStep = false;
        // フレーム送りでは補間待ちの 1 fixed step 遅延を見せず、確定姿勢を表示する。
        m_physicsAlpha = 1.0f;
    } else {
        m_accumulator = std::min(m_accumulator + ctx.dt, maxAccum);
        while (m_accumulator >= fixedDt) {
            runBatches(fixedCtx);
            m_accumulator -= fixedDt;
        }
        // WHAT: 未消化時間が次の固定ステップへどこまで進んだかを描画補間率にする。
        // WHY: 固定物理の段階的な姿勢を可変リフレッシュの描画へ直接露出させないため。
        m_physicsAlpha = fixedDt > 0.0f
            ? std::clamp(m_accumulator / fixedDt, 0.0f, 1.0f)
            : 0.0f;
    }
    ctx.physicsAlpha = m_physicsAlpha;
}

void SystemScheduler::Update(SystemContext ctx)
{
    assert(m_built);
    ctx.physicsAlpha = m_physicsAlpha;
    RunPhase(Phase::PreScript,   ctx);
    RunPhase(Phase::Script,      ctx);
    RunPhase(Phase::PrePhysics,  ctx);
    RunPhase(Phase::Physics,     ctx);
    RunPhase(Phase::PostPhysics, ctx);
    RunPhase(Phase::Navigation,  ctx);
    RunPhase(Phase::LateScript,  ctx);
    RunPhase(Phase::Cleanup,     ctx);
    ++m_frameCounter;
}

void SystemScheduler::LateUpdate(SystemContext ctx)
{
    assert(m_built);
    ctx.physicsAlpha = m_physicsAlpha;
    RunPhase(Phase::LateUpdate, ctx);
}

void SystemScheduler::Shutdown()
{
    for (auto& sys : m_systems)
        sys->OnShutdown();
    m_systems.clear();
    for (auto& b : m_batches) b.clear();
    m_built = false;
}

// ─── デバッグ ──────────────────────────────────────────────────────────────

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
        if (batches.empty()) continue;
        oss << "Phase::" << phaseNames[pi] << "\n";
        for (size_t bi = 0; bi < batches.size(); ++bi) {
            oss << "  Batch[" << bi << "]: ";
            for (size_t si = 0; si < batches[bi].size(); ++si) {
                if (si) oss << " || ";
                oss << batches[bi][si]->Name();
            }
            oss << "\n";
        }
    }
    FBZZ_LOG_INFO("{}", oss.str());
}

} // namespace fbzz
