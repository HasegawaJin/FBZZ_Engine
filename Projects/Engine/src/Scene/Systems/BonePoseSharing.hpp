/// @file    BonePoseSharing.hpp
/// @brief   Bounded frame-scoped memoization of opt-in time-quantized immutable baked poses.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Asset/BakedBoneAnimation.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>

namespace fbzz::scene {
/// @note Time snapping changes only the derived pose; state clocks, speed, events and blend weights remain continuous.
/// @note Assets must remain immutable. Frame/generation boundaries clear the memo; keys include both complete resolved frames.
class BonePoseSharing {
public:
    static asset::BoneBakeStatus Quantize(const asset::BakedBoneAnimation& bake, uint32_t rate, asset::BakedBoneFrame& frame)
    {
        if (rate == 0) return asset::BoneBakeStatus::OK;
        if (rate > 1000 || bake.frameCount < 2 || !std::isfinite(bake.durationSeconds) || bake.durationSeconds <= 0.0
            || frame.first >= bake.frameCount || frame.second != (std::min)(frame.first + 1u,bake.frameCount - 1u)
            || !std::isfinite(frame.alpha) || frame.alpha < 0.0f || frame.alpha > 1.0f)
            return asset::BoneBakeStatus::INVALID_DATA;
        const double intervals = std::ceil(bake.durationSeconds * rate);
        if (!std::isfinite(intervals) || intervals < 1.0 || intervals > 10000000.0)
            return asset::BoneBakeStatus::CAPACITY_EXCEEDED;
        const double normalized = (frame.first + static_cast<double>(frame.alpha)) / (bake.frameCount - 1u);
        const double position = std::clamp(std::round(normalized * intervals) / intervals * (bake.frameCount - 1u),
                                           0.0, static_cast<double>(bake.frameCount - 1u));
        const uint32_t first = static_cast<uint32_t>(std::floor(position));
        frame = {first, (std::min)(first + 1u,bake.frameCount - 1u),static_cast<float>(position - first),0};
        return asset::BoneBakeStatus::OK;
    }
    asset::BoneBakeStatus Evaluate(uint64_t frameSerial, int generation, uint32_t rate,
        const std::shared_ptr<const asset::BakedBoneAnimation>& source, asset::BakedBoneFrame& frame,
        const std::shared_ptr<const asset::BakedBoneAnimation>& target, asset::BakedBoneFrame& targetFrame, float weight,
        std::vector<math::Matrix4>& palette, std::vector<math::Matrix4>& globals,
        asset::BakedBoneWorkspace& workspace, bool& hit)
    {
        hit = false;
        if (!source) return asset::BoneBakeStatus::INVALID_DATA;
        if (m_frame != frameSerial || m_generation != generation || m_rate != rate) {
            m_entries.clear(); m_payloadBytes = 0; m_frame = frameSerial; m_generation = generation; m_rate = rate;
        }
        if (rate == 0) return asset::EvaluateBakedBoneAnimation(*source,frame,palette,globals,target.get(),
                                                               target ? &targetFrame : nullptr,weight,&workspace);
        if (const auto status = Quantize(*source,rate,frame); status != asset::BoneBakeStatus::OK) return status;
        if (target) {
            if (const auto status = Quantize(*target,rate,targetFrame); status != asset::BoneBakeStatus::OK) return status;
        }
        if (!std::isfinite(weight) || weight < 0.0f || weight > 1.0f) return asset::BoneBakeStatus::INVALID_DATA;
        const Key key{source.get(),target.get(),frame.first,frame.second,std::bit_cast<uint32_t>(frame.alpha),
            target ? targetFrame.first : 0,target ? targetFrame.second : 0,
            target ? std::bit_cast<uint32_t>(targetFrame.alpha) : 0,std::bit_cast<uint32_t>(weight)};
        if (const auto existing = m_entries.find(key); existing != m_entries.end()) {
            palette = existing->second.palette; globals = existing->second.globals; hit = true;
            return asset::BoneBakeStatus::OK;
        }
        const auto status = asset::EvaluateBakedBoneAnimation(*source,frame,palette,globals,target.get(),
                                                             target ? &targetFrame : nullptr,weight,&workspace);
        if (status != asset::BoneBakeStatus::OK) return status;
        const uint64_t bytes = static_cast<uint64_t>(palette.size() + globals.size()) * sizeof(math::Matrix4);
        /// @note Entry count also bounds map-node/vector/asset-reference overhead; budget exhaustion keeps the uncached evaluator.
        if (m_entries.size() < 4096 && bytes <= PAYLOAD_BUDGET - m_payloadBytes) {
            m_entries.emplace(key,Value{palette,globals}); m_payloadBytes += bytes;
        }
        return status;
    }
private:
    using Key = std::tuple<const asset::BakedBoneAnimation*,const asset::BakedBoneAnimation*,uint32_t,uint32_t,uint32_t,
                           uint32_t,uint32_t,uint32_t,uint32_t>;
    struct Value {
        std::vector<math::Matrix4> palette,globals;
    };
    static constexpr uint64_t PAYLOAD_BUDGET = 2ull * 1024 * 1024;
    std::map<Key,Value> m_entries;
    uint64_t m_frame = UINT64_MAX, m_payloadBytes = 0;
    int m_generation = -1;
    uint32_t m_rate = 0;
};
}
