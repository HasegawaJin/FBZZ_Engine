/// @file    SequenceCurveEditing.hpp
/// @brief   Float curve editing keeps sorted keys and the selected key's identity.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once

#include <Engine/Asset/AnimationSampling.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace fbzz::editor::detail {

/// @brief Stable sorting returns the new index of the original selected item, including equal times.
template<typename Key, typename Time>
int SortCurveSelection(std::vector<Key>& keys, int selected, Time time)
{
    std::vector<std::size_t> order(keys.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return time(keys[a]) < time(keys[b]);
    });
    std::vector<Key> sorted;
    sorted.reserve(keys.size());
    int result = -1;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (static_cast<int>(order[i]) == selected) result = static_cast<int>(i);
        sorted.push_back(std::move(keys[order[i]]));
    }
    keys = std::move(sorted);
    return result;
}

inline int SortFloatCurve(std::vector<asset::FloatKey>& keys, int selected)
{
    return SortCurveSelection(keys, selected, [](const auto& key) { return key.time; });
}

/// @brief Rejected input leaves the curve and selection unchanged.
inline bool MoveFloatCurveKey(std::vector<asset::FloatKey>& keys, int& selected,
                              double time, float value)
{
    if (selected < 0 || static_cast<std::size_t>(selected) >= keys.size()
        || !std::isfinite(time) || !std::isfinite(value)) return false;
    auto& key = keys[static_cast<std::size_t>(selected)];
    key.time = (std::max)(time, 0.0);
    key.value = value;
    selected = SortFloatCurve(keys, selected);
    return true;
}

inline int AddFloatCurveKey(std::vector<asset::FloatKey>& keys, double time, float value)
{
    if (!std::isfinite(time) || !std::isfinite(value)) return -1;
    keys.push_back({ (std::max)(time, 0.0), value, 0.0f, 0.0f });
    return SortFloatCurve(keys, static_cast<int>(keys.size()) - 1);
}

/// @brief Slopes use value per second, matching the runtime Hermite evaluator.
inline float FloatCurveSecant(const asset::FloatKey& a, const asset::FloatKey& b)
{
    const double span = b.time - a.time;
    if (!(span > 0.0)) return 0.0f;
    const double slope = (static_cast<double>(b.value) - a.value) / span;
    const float result = static_cast<float>(slope);
    return std::isfinite(result) ? result : 0.0f;
}

/// @brief Sets independent incoming/outgoing slopes; equal-time neighbors have flat tangents.
inline void SetFloatCurveLinearTangents(std::vector<asset::FloatKey>& keys, int selected)
{
    if (selected < 0 || static_cast<std::size_t>(selected) >= keys.size()) return;
    const auto i = static_cast<std::size_t>(selected);
    auto& key = keys[i];
    key.inTangent = i > 0 ? FloatCurveSecant(keys[i - 1], key) : 0.0f;
    key.outTangent = i + 1 < keys.size() ? FloatCurveSecant(key, keys[i + 1]) : 0.0f;
}

} /// @note namespace fbzz::editor::detail
