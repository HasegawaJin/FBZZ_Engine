// FBZZ Engine
// RenderGraph.hpp | fbzz::renderer
// Render pass dependency graph with culling, lifetime analysis and profiling hooks
#pragma once

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

    struct ResourceDesc {
        ResourceKind kind = ResourceKind::Unknown;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
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
        // -1.0 = 未計測 (GPU フックが未設定 or まだ latency 待ち)
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
        RenderPass pass;
        pass.name = name;
        pass.accesses.reserve(reads.size() + writes.size());
        for (std::string_view read : reads)
            pass.accesses.push_back({ std::string(read), ResourceUsage::Read });
        for (std::string_view write : writes)
            pass.accesses.push_back({ std::string(write), ResourceUsage::Write });
        pass.execute = std::move(execute);
        pass.allowCulling = allowCulling;
        m_passes.push_back(std::move(pass));
    }

    void AddPass(std::string_view name,
                 std::initializer_list<ResourceAccess> accesses,
                 ExecuteFn execute,
                 bool allowCulling = true)
    {
        RenderPass pass;
        pass.name = name;
        pass.accesses.assign(accesses.begin(), accesses.end());
        pass.execute = std::move(execute);
        pass.allowCulling = allowCulling;
        m_passes.push_back(std::move(pass));
    }

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

    [[nodiscard]] bool Validate() const
    {
        std::vector<size_t> order;
        return BuildExecutionOrder(nullptr, &order);
    }

    [[nodiscard]] bool Execute()
    {
        m_report = {};

        std::vector<size_t> order;
        if (!BuildExecutionOrder(&m_report.culledPasses, &order))
            return false;

        m_report.executionOrder = order;
        m_report.lifetimes = AnalyzeLifetimes(order);

        for (size_t passIndex : order) {
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

    // WHY: CPU フックとは独立した GPU 専用フック。旧実装は SetProfilerHooks を上書きしていた。
    void SetGpuProfilerHooks(std::function<void(std::string_view)> begin,
                             std::function<void(std::string_view)> end)
    {
        m_gpuProfilerBegin = std::move(begin);
        m_gpuProfilerEnd   = std::move(end);
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

    [[nodiscard]] bool BuildExecutionOrder(std::vector<size_t>* culledPasses,
                                           std::vector<size_t>* outOrder) const
    {
        const auto infos = BuildPassInfos();
        std::vector<bool> live(m_passes.size(), m_outputs.empty());

        if (!m_outputs.empty()) {
            std::unordered_set<std::string> required(m_outputs.begin(), m_outputs.end());
            for (size_t i = m_passes.size(); i > 0; --i) {
                const size_t passIndex = i - 1;
                bool needed = !m_passes[passIndex].allowCulling;
                for (const auto& write : infos[passIndex].writes) {
                    if (required.contains(write)) {
                        needed = true;
                        break;
                    }
                }

                if (!needed)
                    continue;

                live[passIndex] = true;
                for (const auto& write : infos[passIndex].writes)
                    required.erase(write);
                for (const auto& read : infos[passIndex].reads)
                    required.insert(read);
            }

            for (const auto& output : m_outputs) {
                bool hasWriter = false;
                for (size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex) {
                    if (!live[passIndex])
                        continue;
                    for (const auto& write : infos[passIndex].writes) {
                        if (write == output) {
                            hasWriter = true;
                            break;
                        }
                    }
                    if (hasWriter)
                        break;
                }
                if (!hasWriter)
                    return false;
            }
        }

        if (culledPasses) {
            for (size_t i = 0; i < live.size(); ++i) {
                if (!live[i])
                    culledPasses->push_back(i);
            }
        }

        std::unordered_map<std::string, size_t> producer;
        std::vector<std::vector<size_t>> edges(m_passes.size());
        std::vector<size_t> indegree(m_passes.size(), 0);

        for (size_t passIndex = 0; passIndex < m_passes.size(); ++passIndex) {
            if (!live[passIndex])
                continue;

            std::unordered_set<size_t> passDeps;
            for (const auto& read : infos[passIndex].reads) {
                auto it = producer.find(read);
                if (it == producer.end()) {
                    if (!IsImportedResource(read))
                        return false;
                    continue;
                }
                if (it->second != passIndex)
                    passDeps.insert(it->second);
            }

            for (size_t dep : passDeps) {
                edges[dep].push_back(passIndex);
                ++indegree[passIndex];
            }

            for (const auto& write : infos[passIndex].writes)
                producer[write] = passIndex;
        }

        std::vector<size_t> ready;
        for (size_t i = 0; i < m_passes.size(); ++i) {
            if (live[i] && indegree[i] == 0)
                ready.push_back(i);
        }

        size_t cursor = 0;
        while (cursor < ready.size()) {
            const size_t passIndex = ready[cursor++];
            outOrder->push_back(passIndex);
            for (size_t next : edges[passIndex]) {
                if (--indegree[next] == 0)
                    ready.push_back(next);
            }
        }

        size_t liveCount = 0;
        for (bool isLive : live) {
            if (isLive)
                ++liveCount;
        }
        return outOrder->size() == liveCount;
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

    [[nodiscard]] static bool CanAlias(const ResourceDesc& a, const ResourceDesc& b)
    {
        return a.kind == b.kind &&
               a.width == b.width &&
               a.height == b.height &&
               a.format == b.format;
    }

    std::vector<RenderPass> m_passes;
    std::vector<std::string> m_outputs;
    std::unordered_map<std::string, ResourceDesc> m_resources;
    ExecutionReport m_report;
    std::function<void(std::string_view)> m_profilerBegin;
    std::function<void(std::string_view)> m_profilerEnd;
    std::function<void(std::string_view)> m_gpuProfilerBegin;
    std::function<void(std::string_view)> m_gpuProfilerEnd;
};

} // namespace fbzz::renderer
