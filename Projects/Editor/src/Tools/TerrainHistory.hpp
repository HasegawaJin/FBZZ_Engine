/// @file    TerrainHistory.hpp
/// @brief   Shared authored terrain snapshots for checked Undo and Redo.
/// @author  Hasegawa Jin
/// @date    2026-11-14
#pragma once

#include "TerrainAuthoring.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <type_traits>

namespace fbzz::editor {

template<typename T>
struct TerrainHistoryBlocks {
    static constexpr std::size_t BLOCK_SIZE = 64 * 1024 / sizeof(T);
    using Block = std::vector<T>;

    std::size_t size = 0;
    std::vector<std::shared_ptr<const Block>> blocks;

    /// @note Baselines only deduplicate storage. Direct writes must be compared even when no revision changed.
    void Capture(std::span<const T> values, const TerrainHistoryBlocks* baseline, const TerrainHistoryBlocks* other)
    {
        size = values.size();
        blocks.reserve((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
        for (std::size_t offset = 0; offset < size; offset += BLOCK_SIZE) {
            const auto section = values.subspan(offset, (std::min)(BLOCK_SIZE, size - offset));
            const auto index = blocks.size();
            const auto blockAt = [index](const TerrainHistoryBlocks* state) {
                return state && index < state->blocks.size() ? state->blocks[index] : nullptr;
            };
            const auto matches = [section](const std::shared_ptr<const Block>& block) {
                return block && section.size() == block->size()
                    && std::memcmp(section.data(), block->data(), section.size_bytes()) == 0;
            };
            auto block = blockAt(baseline);
            if (!matches(block)) {
                auto alternate = blockAt(other);
                if (alternate == block || !matches(alternate))
                    alternate = std::make_shared<Block>(section.begin(), section.end());
                block = std::move(alternate);
            }
            blocks.push_back(std::move(block));
        }
    }

    [[nodiscard]] bool Matches(std::span<const T> values) const
    {
        if (values.size() != size) return false;
        std::size_t offset = 0;
        for (const auto& block : blocks) {
            const auto section = values.subspan(offset, block->size());
            if constexpr (std::is_same_v<T, float>) {
                if (!TerrainAuthoringHeightsEqual(section, *block)) return false;
            } else if (std::memcmp(section.data(), block->data(), section.size_bytes()) != 0) return false;
            offset += block->size();
        }
        return true;
    }

    [[nodiscard]] Block Materialize() const
    {
        Block values;
        values.reserve(size);
        for (const auto& block : blocks) values.insert(values.end(), block->begin(), block->end());
        return values;
    }

    [[nodiscard]] T At(std::size_t index) const { return (*blocks[index / BLOCK_SIZE])[index % BLOCK_SIZE]; }

    /// @pre Edits are ordered by valid grid index.
    template<typename Edits, typename Value>
    void Apply(const Edits& edits, const Value& value)
    {
        std::shared_ptr<Block> changed;
        std::size_t changedIndex = blocks.size();
        for (const auto& edit : edits) {
            const auto index = edit.index / BLOCK_SIZE;
            if (index != changedIndex) {
                changedIndex = index;
                changed = std::make_shared<Block>(*blocks[index]);
                blocks[index] = changed;
            }
            (*changed)[edit.index % BLOCK_SIZE] = value(edit);
        }
    }
};

struct TerrainHistorySnapshot {
    scene::TerrainComponent metadata;
    TerrainHistoryBlocks<float> heights;
    TerrainHistoryBlocks<std::uint8_t> indices, weights, holes;
    std::uint64_t lifetime;

    TerrainHistorySnapshot(const scene::TerrainComponent& source, std::uint64_t token,
        const TerrainHistorySnapshot* baseline, const TerrainHistorySnapshot* other = nullptr);

    [[nodiscard]] bool Matches(const scene::TerrainComponent& current) const;
    [[nodiscard]] scene::TerrainComponent Materialize() const;
};

} /// @note namespace fbzz::editor
