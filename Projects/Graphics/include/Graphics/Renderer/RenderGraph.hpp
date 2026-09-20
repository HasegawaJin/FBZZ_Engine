/// @file    RenderGraph.hpp
/// @brief   Render pass dependency graph with scheduling, culling, lifetime analysis and profiling hooks.
/// @author  Hasegawa Jin
/// @date    2026-05-26
/// @note 実行順は «申告された依存» から導く。登録順は 2 つの役目しか持たない。
/// @note   1. 同じリソースへ複数のパスが書くときの世代の順序 (Sky は Geometry の上に描く)
/// @note   2. 依存が何も言っていないときのタイブレーク (SchedulePolicy::RegistrationOrder)
/// @note どちらも «意図» であって偶然ではないので、そこだけは登録順に従う。それ以外の順序は
/// @note 依存が決めるため、生産者を消費者より後ろに登録しても正しく並ぶ。
#pragma once

#include <Graphics/Renderer/Format.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::renderer {

class RenderGraph {
public:
    using ExecuteFn = std::function<void()>;

    enum class ResourceKind {
        Unknown,
        RenderTarget,
        Texture,
        Buffer
    };

    enum class ResourceUsage {
        Read,
        Write,
        ReadWrite
    };

    /// @note 依存を満たす実行順が複数あるとき、どれを選ぶかの方針。
    /// @note     /// どちらを選んでも «依存として申告された制約» は必ず守られる。違うのは、制約が
    /// @note 何も言っていない部分をどう埋めるか。
    enum class SchedulePolicy {
        /// @note 実行可能なパスのうち常に登録順が最小のものを出す。既定。
        /// @note 申告が正しい限り実行順は登録順に一致するので、既存のパイプラインは動かない。
        RegistrationOrder,
        /// @note 生きているリソースの本数をなるべく低く保つ。同時に生存する中間 RT が減るので
        /// @note エイリアスが効きやすくなる代わりに、実行順が登録順から離れる。
        /// @note 申告漏れのあるパス (読むと言っていないリソースを束縛するパス) は即座に壊れる。
        MinimizeLifetimes
    };

    /// @note リソースの «形»。エイリアシングはこれが一致するかどうかだけで判断する。
    /// @note colorCount と withDepth まで持つのは、寸法と形式だけでは深度専用 RT (colorCount=0)
    /// @note       とカラーマスクが «同型» と誤判定されていたため。実体の作られ方に効く項目が
    /// @note       抜けていると、貸し回した先で別物になる。
    struct ResourceDesc {
        ResourceKind kind = ResourceKind::Unknown;
        uint32_t width = 0;
        uint32_t height = 0;
        Format   format = Format::RGBA16F;
        /// @note RenderTarget の同時出力カラー数。0 = 深度専用。Texture / Buffer では 1。
        uint32_t colorCount = 1;
        /// @note 深度バッファを持つか。RenderTarget 以外では false。
        bool withDepth = false;
        bool external = false;
        bool transient = true;
    };

    struct ResourceAccess {
        std::string name;
        ResourceUsage usage = ResourceUsage::Read;
    };

    struct RenderPass {
        std::string name;
        std::vector<ResourceAccess> accesses;
        ExecuteFn execute;
        bool allowCulling = true;

        [[nodiscard]] std::vector<std::string> Reads() const
        {
            std::vector<std::string> result;
            for (const auto& access : accesses) {
                if (access.usage == ResourceUsage::Read || access.usage == ResourceUsage::ReadWrite)
                    result.push_back(access.name);
            }
            return result;
        }

        [[nodiscard]] std::vector<std::string> Writes() const
        {
            std::vector<std::string> result;
            for (const auto& access : accesses) {
                if (access.usage == ResourceUsage::Write || access.usage == ResourceUsage::ReadWrite)
                    result.push_back(access.name);
            }
            return result;
        }
    };

    struct ResourceLifetime {
        std::string name;
        int firstPass = -1;
        int lastPass = -1;
        int aliasGroup = -1;
        ResourceDesc desc;
    };

    struct PassProfile {
        std::string name;
        double cpuMilliseconds = 0.0;
        /// @note -1.0 = 未計測 (GPU フックが未設定 or まだ latency 待ち)
        double gpuMilliseconds = -1.0;
    };

    struct ExecutionReport {
        std::vector<size_t> executionOrder;
        std::vector<size_t> culledPasses;
        std::vector<ResourceLifetime> lifetimes;
        std::vector<PassProfile> profiles;
    };

    void Clear()
    {
        m_passes.clear();
        m_outputs.clear();
        m_resources.clear();
        m_report = {};
    }

    void AddPass(std::string_view name,
                 std::initializer_list<std::string_view> reads,
                 std::initializer_list<std::string_view> writes,
                 ExecuteFn execute)
    {
        AddPass(name, reads, writes, std::move(execute), true);
    }

    void AddPass(std::string_view name,
                 std::initializer_list<std::string_view> reads,
                 std::initializer_list<std::string_view> writes,
                 ExecuteFn execute,
                 bool allowCulling)
    {
        std::vector<ResourceAccess> accesses;
        accesses.reserve(reads.size() + writes.size());
        for (std::string_view read : reads)
            accesses.push_back({ std::string(read), ResourceUsage::Read });
        for (std::string_view write : writes)
            accesses.push_back({ std::string(write), ResourceUsage::Write });
        AddPass(name, std::move(accesses), std::move(execute), allowCulling);
    }

    void AddPass(std::string_view name,
                 std::initializer_list<ResourceAccess> accesses,
                 ExecuteFn execute,
                 bool allowCulling = true)
    {
        AddPass(name, std::vector<ResourceAccess>(accesses.begin(), accesses.end()),
                std::move(execute), allowCulling);
    }

    /// @note すべてのオーバーロードはここへ集まる。
    void AddPass(std::string_view name,
                 std::vector<ResourceAccess> accesses,
                 ExecuteFn execute,
                 bool allowCulling = true)
    {
        RenderPass pass;
        pass.name = name;
        pass.accesses = std::move(accesses);
        pass.execute = std::move(execute);
        pass.allowCulling = allowCulling;
        m_passes.push_back(std::move(pass));
    }

    void DeclareResource(std::string_view name, const ResourceDesc& desc)
    {
        m_resources[std::string(name)] = desc;
    }

    void SetOutputs(std::initializer_list<std::string_view> outputs)
    {
        m_outputs.clear();
        m_outputs.reserve(outputs.size());
        for (std::string_view output : outputs)
            m_outputs.push_back(std::string(output));
    }

    /// @note RenderPipeline など動的にパスを構築する側から呼ぶ。SetOutputs のクリア不要版。
    void AddOutput(std::string_view name)
    {
        m_outputs.emplace_back(name);
    }

    [[nodiscard]] bool Validate() const
    {
        std::vector<size_t> order;
        return BuildExecutionOrder(nullptr, &order);
    }

    /// @note Plan: パス実行なしで依存解決・カリング・ライフタイム解析だけを行う。
    /// @note TransientRTPool が Execute() より前にリソース割り当てを確定できるよう、何が実行され
    /// @note       どのリソースがどのパス間で生きているかを事前に知る必要がある。Plan() → プール再構築
    /// @note       → Execute() の 2 フェーズにすることで、Execute 時にはトランジェント RT が確保済みになる。
    [[nodiscard]] bool Plan()
    {
        m_report = {};

        std::vector<size_t> order;
        if (!BuildExecutionOrder(&m_report.culledPasses, &order))
            return false;

        m_report.executionOrder = order;
        m_report.lifetimes      = AnalyzeLifetimes(order);
        return true;
    }

    /// @note 外部で計算済みの ExecutionReport を注入して次回 Execute() の Plan() をスキップさせる。
    /// @note profiles は Execute() が毎回書き直すためここでクリアする。
    void InjectPlan(ExecutionReport rep)
    {
        rep.profiles.clear();
        m_report = std::move(rep);
    }

    /// @note Execute: Plan() 済みの実行順でパスコールバックを呼ぶ。
    /// @note Plan() が未呼び出しの場合は内部で Plan() を実行してから進む。
    [[nodiscard]] bool Execute()
    {
        /// @note 前フレームのプランが残っていない (executionOrder 空) 場合は Plan を走らせる。
        if (m_report.executionOrder.empty()) {
            if (!Plan()) return false;
        }

        for (size_t passIndex : m_report.executionOrder) {
            auto& pass = m_passes[passIndex];
            if (pass.execute) {
                const auto start = std::chrono::steady_clock::now();
                if (m_profilerBegin)
                    m_profilerBegin(pass.name);
                if (m_gpuProfilerBegin)
                    m_gpuProfilerBegin(pass.name);

                pass.execute();

                if (m_gpuProfilerEnd)
                    m_gpuProfilerEnd(pass.name);
                if (m_profilerEnd)
                    m_profilerEnd(pass.name);
                const auto end = std::chrono::steady_clock::now();
                const std::chrono::duration<double, std::milli> elapsed = end - start;
                m_report.profiles.push_back({ pass.name, elapsed.count() });
                if (m_passCompleted)
                    m_passCompleted(passIndex);
            }
        }

        return true;
    }

    [[nodiscard]] const std::vector<RenderPass>& GetPasses() const
    {
        return m_passes;
    }

    [[nodiscard]] const ExecutionReport& GetLastReport() const
    {
        return m_report;
    }

    void SetProfilerHooks(std::function<void(std::string_view)> begin,
                          std::function<void(std::string_view)> end)
    {
        m_profilerBegin = std::move(begin);
        m_profilerEnd = std::move(end);
    }

    /// @note 計測終了後、次のパスによる上書き前に呼ぶ。カリングされたパスでは呼ばれない。
    /// @note コールバックからグラフを変更しないこと。
    void SetPassCompletedHook(std::function<void(size_t)> hook) { m_passCompleted = std::move(hook); }

    /// @note CPU フックとは独立した GPU 専用フック。旧実装は SetProfilerHooks を上書きしていた。
    void SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                             std::function<void(std::string_view)> end)
    {
        m_gpuProfilerBegin = std::move(begin);
        m_gpuProfilerEnd   = std::move(end);
    }

    /// @note Plan() 失敗時にどのリソース依存が壊れているかを報告するデバッグフック。
    void SetDebugLogHook(std::function<void(const char*)> fn) { m_debugLog = std::move(fn); }

    /// @note 実行順の決め方を切り替える。次の Plan() から効く。
    void SetSchedulePolicy(SchedulePolicy policy) { m_policy = policy; }

    [[nodiscard]] SchedulePolicy GetSchedulePolicy() const { return m_policy; }

    [[nodiscard]] static const char* DescribeFormat(Format format)
    {
        switch (format) {
        case Format::RGBA16F:    return "RGBA16F";
        case Format::RGBA8:      return "RGBA8";
        case Format::R11G11B10F: return "R11G11B10F";
        case Format::RG16F:      return "RG16F";
        case Format::R16F:       return "R16F";
        case Format::R8:         return "R8";
        }
        return "?";
    }

    [[nodiscard]] static const char* DescribeKind(ResourceKind kind)
    {
        switch (kind) {
        case ResourceKind::RenderTarget: return "RenderTarget";
        case ResourceKind::Texture:      return "Texture";
        case ResourceKind::Buffer:       return "Buffer";
        case ResourceKind::Unknown:      break;
        }
        return "Unknown";
    }

    /// @note 直前の Plan の結果を «差分の取れる» テキストにする。
    /// @note 実行順やエイリアスの割り当てが変わったことは絵を見ても分からない。パスを作り替える
    /// @note       改修で «絵は同じだが順序が変わった» を捕まえられる唯一の手掛かり。計測値は入れない
    /// @note       (毎フレーム変わるので差分が意味を失う)。
    [[nodiscard]] std::string DescribeLastPlan() const
    {
        const auto passName = [this](size_t passIndex) -> std::string {
            return passIndex < m_passes.size() ? m_passes[passIndex].name : std::string("<invalid>");
        };

        std::string out = "policy ";
        out += (m_policy == SchedulePolicy::MinimizeLifetimes) ? "MinimizeLifetimes"
                                                               : "RegistrationOrder";
        out += '\n';

        /// @note '+' で繋がず += を並べるのは、const char* / std::string / char が混ざる長い連鎖だと
        /// @note       オーバーロード解決の候補が組み合わせで膨らむため。実際に MSVC が「'+' を std::string
        /// @note       に適用できない」と落ちた。+= の並びなら 1 手ずつ解決され足す順序もそのまま見える。
        for (size_t index = 0; index < m_report.executionOrder.size(); ++index) {
            out += "pass ";
            out += std::to_string(index);
            out += ' ';
            out += passName(m_report.executionOrder[index]);
            out += '\n';
        }

        for (const size_t passIndex : m_report.culledPasses) {
            out += "culled ";
            out += passName(passIndex);
            out += '\n';
        }

        for (const auto& lifetime : m_report.lifetimes) {
            out += "res ";
            out += lifetime.name;
            out += " first=";
            out += std::to_string(lifetime.firstPass);
            out += " last=";
            out += std::to_string(lifetime.lastPass);
            out += " alias=";
            out += std::to_string(lifetime.aliasGroup);
            out += " kind=";
            out += DescribeKind(lifetime.desc.kind);
            out += " size=";
            out += std::to_string(lifetime.desc.width);
            out += 'x';
            out += std::to_string(lifetime.desc.height);
            out += " fmt=";
            out += DescribeFormat(lifetime.desc.format);
            out += " colors=";
            out += std::to_string(lifetime.desc.colorCount);
            if (lifetime.desc.withDepth) out += " depth";
            if (lifetime.desc.external)  out += " external";
            if (lifetime.desc.transient) out += " transient";
            out += '\n';
        }

        return out;
    }

private:
    struct PassInfo {
        std::vector<std::string> reads;
        std::vector<std::string> writes;
    };

    [[nodiscard]] std::vector<PassInfo> BuildPassInfos() const
    {
        std::vector<PassInfo> infos;
        infos.reserve(m_passes.size());
        for (const auto& pass : m_passes) {
            PassInfo info;
            for (const auto& access : pass.accesses) {
                if (access.usage == ResourceUsage::Read || access.usage == ResourceUsage::ReadWrite)
                    info.reads.push_back(access.name);
                if (access.usage == ResourceUsage::Write || access.usage == ResourceUsage::ReadWrite)
                    info.writes.push_back(access.name);
            }
            infos.push_back(std::move(info));
        }
        return infos;
    }

    /// @note 依存グラフを «登録順から独立に» 組み、実行順を導く。
    /// @note     /// 同じリソースへ複数のパスが書く場合、その «世代» の順序だけは登録順が決める。
    /// @note これは事故ではなく仕様そのもの (Sky は Geometry の上に描く) なので動かさない。
    /// @note 動かせるのは «世代が競合しないパス同士» の相対順序で、そこはスケジューラーが決める。
    [[nodiscard]] bool BuildExecutionOrder(std::vector<size_t>* culledPasses,
                                           std::vector<size_t>* outOrder) const
    {
        const size_t passCount = m_passes.size();
        const auto infos = BuildPassInfos();

        /// @name 1. 書き手を登録順に集める。これがリソースの世代になる
        std::unordered_map<std::string, std::vector<size_t>> writersOf;
        for (size_t pass = 0; pass < passCount; ++pass) {
            for (const auto& write : infos[pass].writes) {
                auto& writers = writersOf[write];
                if (writers.empty() || writers.back() != pass)
                    writers.push_back(pass);
            }
        }

        /// @name 2. 読み手を世代へ束ね、RAW の依存を張る
        /// @note readersOfGeneration[R][g] = 世代 g を読むパス。g == 0 はフレーム開始時の中身。
        std::vector<std::vector<size_t>> rawDeps(passCount);
        std::unordered_map<std::string, std::vector<std::vector<size_t>>> readersOfGeneration;
        for (const auto& [name, writers] : writersOf)
            readersOfGeneration[name].resize(writers.size() + 1);

        /// @note 生産者の居ない読み取りは «記録するだけ» にして生存判定の後に生きているパスのぶんだけ
        /// @note       落とす。刈られるパスの申告漏れまで Plan を失敗させると設定を切った途端に画面が出なく
        /// @note       なるため、実際に実行されるパスの申告だけを契約とする。
        std::vector<std::pair<size_t, std::string>> unresolvedReads;

        for (size_t pass = 0; pass < passCount; ++pass) {
            for (const auto& read : infos[pass].reads) {
                const auto it = writersOf.find(read);
                if (it == writersOf.end()) {
                    if (!IsImportedResource(read))
                        unresolvedReads.emplace_back(pass, read);
                    continue;
                }

                const auto& writers = it->second;
                size_t generation = 0;
                for (size_t index = 0; index < writers.size() && writers[index] < pass; ++index)
                    generation = index + 1;

                /// @note 自分より前に書き手が居ない場合。imported ならフレーム開始時の中身を
                /// @note       読む意図として通す。そうでなければ最初の書き手へ繋ぐ ── ここが
                /// @note       «並べ替え» の入口で、登録位置が生産者より前でも順序を作れる。
                if (generation == 0 && !IsImportedResource(read))
                    generation = 1;

                if (generation > 0) {
                    const size_t producer = writers[generation - 1];
                    if (producer != pass)
                        rawDeps[pass].push_back(producer);
                }
                readersOfGeneration[read][generation].push_back(pass);
            }
        }

        /// @name 3. 生存判定。出力へ «データが流れ込むか» だけで決める
        /// @note RAW だけを辿るのは、WAW/WAR が «順序» の制約であって «必要性» ではないため。
        /// @note       後続が全面的に上書きするパスは、その結果を誰も読まないなら要らない。
        std::vector<bool> live(passCount, m_outputs.empty());
        if (!m_outputs.empty()) {
            std::vector<size_t> pending;
            const auto mark = [&live, &pending](size_t pass) {
                if (live[pass])
                    return;
                live[pass] = true;
                pending.push_back(pass);
            };

            for (size_t pass = 0; pass < passCount; ++pass) {
                if (!m_passes[pass].allowCulling)
                    mark(pass);
            }

            for (const auto& output : m_outputs) {
                const auto it = writersOf.find(output);
                if (it == writersOf.end() || it->second.empty()) {
                    /// @note デバッグ用。どの output resource に writer がいないか特定する。
                    if (m_debugLog)
                        m_debugLog(("RenderGraph: no live pass writes required output \""
                            + output + "\"").c_str());
                    return false;
                }
                /// @note 出力に残るのは最後の世代。それを作る書き手が起点になる。
                mark(it->second.back());
            }

            while (!pending.empty()) {
                const size_t pass = pending.back();
                pending.pop_back();
                for (const size_t dep : rawDeps[pass])
                    mark(dep);
            }
        }

        for (const auto& [pass, read] : unresolvedReads) {
            if (!live[pass])
                continue;
            /// @note デバッグ用。どのリソースに producer がいないか特定する。
            if (m_debugLog)
                m_debugLog(("RenderGraph: pass \"" + m_passes[pass].name
                    + "\" reads \"" + read
                    + "\" but no pass writes it and it is not imported.").c_str());
            return false;
        }

        if (culledPasses) {
            for (size_t pass = 0; pass < passCount; ++pass) {
                if (!live[pass])
                    culledPasses->push_back(pass);
            }
        }

        /// @name 4. 辺を張る (生きているパスだけ)
        std::vector<std::unordered_set<size_t>> dependencies(passCount);
        const auto addEdge = [&dependencies, &live](size_t from, size_t to) {
            if (from == to || !live[from] || !live[to])
                return;
            dependencies[to].insert(from);
        };

        for (size_t pass = 0; pass < passCount; ++pass) {
            if (!live[pass])
                continue;
            for (const size_t dep : rawDeps[pass])
                addEdge(dep, pass);
        }

        for (const auto& [name, writers] : writersOf) {
            /// @note WAW: 世代の順序は登録順が決める。生きている書き手だけを鎖にする。
            std::vector<size_t> liveWriters;
            for (const size_t writer : writers) {
                if (live[writer])
                    liveWriters.push_back(writer);
            }
            for (size_t index = 1; index < liveWriters.size(); ++index)
                addEdge(liveWriters[index - 1], liveWriters[index]);

            /// @note WAR: 世代 g を読むパスは、次の世代を書くパスより前に置く。
            const auto& generations = readersOfGeneration.at(name);
            for (size_t generation = 0; generation < writers.size(); ++generation) {
                for (const size_t reader : generations[generation])
                    addEdge(reader, writers[generation]);
            }
        }

        std::vector<std::vector<size_t>> edges(passCount);
        std::vector<size_t> indegree(passCount, 0);
        for (size_t pass = 0; pass < passCount; ++pass) {
            for (const size_t dep : dependencies[pass]) {
                edges[dep].push_back(pass);
                ++indegree[pass];
            }
        }
        /// @note dependencies は unordered_set なので走査順が決まらない。ここで登録順へ揃える。
        for (auto& targets : edges)
            std::sort(targets.begin(), targets.end());

        /// @name 5. トポロジカルソート
        size_t liveCount = 0;
        for (const bool isLive : live) {
            if (isLive)
                ++liveCount;
        }

        std::vector<size_t> ready;
        for (size_t pass = 0; pass < passCount; ++pass) {
            if (live[pass] && indegree[pass] == 0)
                ready.push_back(pass);
        }

        /// @note MinimizeLifetimes の帳簿。remainingUses が 0 になった時点でそのリソースは死ぬ。
        std::unordered_map<std::string, size_t> remainingUses;
        std::vector<std::vector<std::string>> touches(passCount);
        if (m_policy == SchedulePolicy::MinimizeLifetimes) {
            for (size_t pass = 0; pass < passCount; ++pass) {
                if (!live[pass])
                    continue;
                std::unordered_set<std::string> unique;
                for (const auto& read : infos[pass].reads)
                    unique.insert(read);
                for (const auto& write : infos[pass].writes)
                    unique.insert(write);
                touches[pass].assign(unique.begin(), unique.end());
                std::sort(touches[pass].begin(), touches[pass].end());
                for (const auto& name : touches[pass])
                    ++remainingUses[name];
            }
        }
        std::unordered_set<std::string> aliveResources;

        outOrder->reserve(liveCount);
        while (!ready.empty()) {
            size_t chosen = 0;
            if (m_policy == SchedulePolicy::MinimizeLifetimes) {
                long long bestScore = 0;
                for (size_t index = 0; index < ready.size(); ++index) {
                    long long allocated = 0;
                    long long released  = 0;
                    for (const auto& name : touches[ready[index]]) {
                        if (!aliveResources.contains(name))
                            ++allocated;
                        if (remainingUses[name] == 1)
                            ++released;
                    }
                    const long long score = allocated - released;
                    if (index == 0 || score < bestScore
                        || (score == bestScore && ready[index] < ready[chosen])) {
                        bestScore = score;
                        chosen    = index;
                    }
                }
            } else {
                /// @note 依存が同じなら常に登録順が最小のものを出す。
                /// @note       これにより «申告が正しければ実行順は登録順に一致する» が保たれる。
                for (size_t index = 1; index < ready.size(); ++index) {
                    if (ready[index] < ready[chosen])
                        chosen = index;
                }
            }

            const size_t pass = ready[chosen];
            ready[chosen] = ready.back();
            ready.pop_back();
            outOrder->push_back(pass);

            if (m_policy == SchedulePolicy::MinimizeLifetimes) {
                for (const auto& name : touches[pass]) {
                    aliveResources.insert(name);
                    if (--remainingUses[name] == 0)
                        aliveResources.erase(name);
                }
            }

            for (const size_t next : edges[pass]) {
                if (--indegree[next] == 0)
                    ready.push_back(next);
            }
        }

        if (outOrder->size() != liveCount) {
            /// @note 依存が循環すると indegree が 0 に戻らないパスが残る。
            if (m_debugLog)
                m_debugLog("RenderGraph: dependency cycle detected - some passes never became ready.");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool IsImportedResource(const std::string& name) const
    {
        auto it = m_resources.find(name);
        return it != m_resources.end() && (it->second.external || !it->second.transient);
    }

    [[nodiscard]] std::vector<ResourceLifetime> AnalyzeLifetimes(const std::vector<size_t>& order) const
    {
        const auto infos = BuildPassInfos();
        std::unordered_map<std::string, ResourceLifetime> byName;
        for (size_t orderIndex = 0; orderIndex < order.size(); ++orderIndex) {
            const size_t passIndex = order[orderIndex];
            auto touch = [&](const std::string& name) {
                auto& lt = byName[name];
                lt.name = name;
                if (lt.firstPass < 0)
                    lt.firstPass = static_cast<int>(orderIndex);
                lt.lastPass = static_cast<int>(orderIndex);
                if (auto desc = m_resources.find(name); desc != m_resources.end())
                    lt.desc = desc->second;
            };

            for (const auto& read : infos[passIndex].reads)
                touch(read);
            for (const auto& write : infos[passIndex].writes)
                touch(write);
        }

        std::vector<ResourceLifetime> lifetimes;
        lifetimes.reserve(byName.size());
        for (auto& [_, lifetime] : byName)
            lifetimes.push_back(std::move(lifetime));
        std::sort(lifetimes.begin(), lifetimes.end(),
            [](const ResourceLifetime& a, const ResourceLifetime& b) {
                if (a.firstPass != b.firstPass)
                    return a.firstPass < b.firstPass;
                return a.name < b.name;
            });

        struct AliasGroupState {
            int lastPass = (std::numeric_limits<int>::min)();
            ResourceDesc desc;
        };
        std::vector<AliasGroupState> groups;
        for (auto& lifetime : lifetimes) {
            if (lifetime.desc.external || !lifetime.desc.transient) {
                lifetime.aliasGroup = -1;
                continue;
            }

            int chosen = -1;
            for (size_t group = 0; group < groups.size(); ++group) {
                if (groups[group].lastPass < lifetime.firstPass &&
                    CanAlias(groups[group].desc, lifetime.desc)) {
                    chosen = static_cast<int>(group);
                    break;
                }
            }

            if (chosen < 0) {
                chosen = static_cast<int>(groups.size());
                groups.push_back({ (std::numeric_limits<int>::min)(), lifetime.desc });
            }

            lifetime.aliasGroup = chosen;
            groups[static_cast<size_t>(chosen)].lastPass = lifetime.lastPass;
        }

        return lifetimes;
    }

    /// @note 貸し回してよいのは «実体の作られ方が完全に同じ» ときだけ。
    /// @note 1 項目でも違えば別グループにする (寸法だけ見ていた頃の取りこぼしを塞ぐ)。
    [[nodiscard]] static bool CanAlias(const ResourceDesc& a, const ResourceDesc& b)
    {
        return a.kind == b.kind &&
               a.width == b.width &&
               a.height == b.height &&
               a.format == b.format &&
               a.colorCount == b.colorCount &&
               a.withDepth == b.withDepth;
    }

    std::vector<RenderPass> m_passes;
    std::vector<std::string> m_outputs;
    std::unordered_map<std::string, ResourceDesc> m_resources;
    ExecutionReport m_report;
    std::function<void(std::string_view)> m_profilerBegin;
    std::function<void(size_t)> m_passCompleted;
    std::function<void(std::string_view)> m_profilerEnd;
    std::function<void(std::string_view)> m_gpuProfilerBegin;
    std::function<void(std::string_view)> m_gpuProfilerEnd;
    std::function<void(const char*)>      m_debugLog;  ///< @note Plan() 失敗診断用
    SchedulePolicy                        m_policy = SchedulePolicy::RegistrationOrder;
};

} /// @note namespace fbzz::renderer
