// FBZZ Engine
// RenderGraph.hpp | fbzz::renderer
// Lightweight render-pass graph with resource IO metadata
#pragma once

#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::renderer {

class RenderGraph {
public:
    using ExecuteFn = std::function<void()>;

    struct RenderPass {
        std::string name;
        std::vector<std::string> reads;
        std::vector<std::string> writes;
        ExecuteFn execute;
    };

    void Clear()
    {
        m_passes.clear();
    }

    void AddPass(std::string_view name,
                 std::initializer_list<std::string_view> reads,
                 std::initializer_list<std::string_view> writes,
                 ExecuteFn execute)
    {
        RenderPass pass;
        pass.name = name;
        pass.reads.assign(reads.begin(), reads.end());
        pass.writes.assign(writes.begin(), writes.end());
        pass.execute = std::move(execute);
        m_passes.push_back(std::move(pass));
    }

    [[nodiscard]] bool Validate() const
    {
        std::unordered_set<std::string> produced;
        for (const auto& pass : m_passes) {
            for (const auto& read : pass.reads) {
                if (!produced.contains(read)) {
                    return false;
                }
            }

            for (const auto& write : pass.writes) {
                produced.insert(write);
            }
        }

        return true;
    }

    [[nodiscard]] bool Execute()
    {
        if (!Validate()) return false;

        for (auto& pass : m_passes) {
            if (pass.execute) {
                pass.execute();
            }
        }

        return true;
    }

    [[nodiscard]] const std::vector<RenderPass>& GetPasses() const
    {
        return m_passes;
    }

private:
    std::vector<RenderPass> m_passes;
};

} // namespace fbzz::renderer
