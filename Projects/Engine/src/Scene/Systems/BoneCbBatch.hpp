/// @file    BoneCbBatch.hpp
/// @brief   Reused immutable full-palette packets over the existing resource-update executor.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include "AnimationWorkTrace.hpp"
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>

namespace fbzz::scene {
class BoneCbBatch {
public:
    BoneCbBatch(renderer::ResourceManager& resources, bool enabled, size_t actorHint)
        : m_resources(resources), m_enabled(enabled && resources.DefersResourceUpdates()), m_actorHint(actorHint)
    { m_previous = s_current; s_current = this; }
    ~BoneCbBatch() { Flush(); s_current = m_previous; }
    void SetActorEligible(bool eligible)
    {
        if (!eligible) Flush();
        m_actorEligible = m_enabled && eligible;
    }
    static void Update(renderer::ResourceManager& resources,
        renderer::ResourceHandle<renderer::ConstantBufferTag> handle, const void* data, size_t size)
    {
        CountAnimationCompatibilityUpdate(size);
        if (s_current && &s_current->m_resources == &resources && s_current->Add(handle,data,size)) return;
        resources.Update(handle,data,size);
    }
    static void UpdatePalette(renderer::ResourceManager& resources,
        renderer::ResourceHandle<renderer::ConstantBufferTag> handle,const std::vector<math::Matrix4>& matrices)
    {
        constexpr size_t size = asset::MAX_SKINNING_BONES * sizeof(math::Matrix4);
        CountAnimationCompatibilityUpdate(size);
        if (s_current && &s_current->m_resources == &resources && s_current->AddPalette(handle,matrices)) return;
        /// @note Byte storage avoids constructing 128 matrices for every compatibility CB; all identity tail bytes remain exact.
        std::array<uint8_t,size> data;
        const auto& identity=IdentityPalette();
        std::memcpy(data.data(),identity.data(),size);
        const size_t prefix=(std::min)(matrices.size(),identity.size())*sizeof(math::Matrix4);
        if(prefix) std::memcpy(data.data(),matrices.data(),prefix);
        resources.Update(handle,data.data(),size);
    }
    /// @note Flush before creating/applying CPU bone objects or entering any legacy fallback callback path.
    static void BeforeBoneHierarchy(renderer::ResourceManager& resources)
    {
        if (s_current && &s_current->m_resources == &resources) s_current->SetActorEligible(false);
    }
    BoneCbBatch(const BoneCbBatch&) = delete;
    BoneCbBatch& operator=(const BoneCbBatch&) = delete;
private:
    /// @note A queued packet holds its own strong reference. Reuse is allowed only after all renderer consumers release it.
    static std::shared_ptr<renderer::ConstantBufferUpdateBatch> Acquire(size_t hint)
    {
        static std::mutex mutex;
        static std::array<std::shared_ptr<renderer::ConstantBufferUpdateBatch>,3> packets;
        std::lock_guard lock(mutex);
        for (auto& packet : packets) {
            if (!packet) packet = std::make_shared<renderer::ConstantBufferUpdateBatch>();
            if (packet.use_count() != 1) continue;
            packet->entries.clear(); packet->bytes.clear();
            const size_t rows = (std::min)(hint,static_cast<size_t>(4096));
            packet->entries.reserve(rows); packet->bytes.reserve((std::min)(rows * 8192,MAX_PACKET_BYTES));
            return packet;
        }
        return {};
    }
    bool Add(renderer::ResourceHandle<renderer::ConstantBufferTag> handle, const void* data, size_t size)
    {
        if (!m_actorEligible || !data || size == 0 || size > MAX_PACKET_BYTES) return false;
        if (!m_packet) m_packet = Acquire(m_actorHint * 2 + 16);
        if (!m_packet) { m_actorEligible = false; return false; }
        if (size > MAX_PACKET_BYTES - m_packet->bytes.size()) {
            Flush(); m_actorEligible = m_enabled; m_packet = Acquire(m_actorHint * 2 + 16);
            if (!m_packet) { m_actorEligible = false; return false; }
        }
        const size_t offset = m_packet->bytes.size();
        m_packet->bytes.resize(offset + size);
        std::memcpy(m_packet->bytes.data() + offset,data,size);
        m_packet->entries.push_back({handle,offset,size}); return true;
    }
    static const std::array<math::Matrix4,asset::MAX_SKINNING_BONES>& IdentityPalette()
    {
        static const auto identity=[] {
            std::array<math::Matrix4,asset::MAX_SKINNING_BONES> value{};
            for(auto& matrix:value) matrix=math::Matrix4::Identity();
            return value;
        }();
        return identity;
    }
    bool AddPalette(renderer::ResourceHandle<renderer::ConstantBufferTag> handle,const std::vector<math::Matrix4>& matrices)
    {
        if (!m_actorEligible) return false;
        const auto& identity=IdentityPalette();
        if (!Add(handle,identity.data(),sizeof(identity))) return false;
        const size_t offset = m_packet->entries.back().offset;
        const size_t bytes = (std::min)(matrices.size(),identity.size()) * sizeof(math::Matrix4);
        if (bytes) std::memcpy(m_packet->bytes.data() + offset,matrices.data(),bytes);
        return true;
    }
    void Flush()
    {
        if (!m_packet || m_packet->entries.empty()) { m_packet.reset(); return; }
        AnimationWorkScope scope(AnimationWorkPart::BATCH_SUBMIT);
        m_resources.UpdateConstantBuffers(m_packet);
        if (auto* row = CurrentAnimationWorkRow()) ++row->cbBulkPackets;
        m_packet.reset();
    }
    static constexpr size_t MAX_PACKET_BYTES = 32ull * 1024 * 1024;
    inline static thread_local BoneCbBatch* s_current = nullptr;
    BoneCbBatch* m_previous = nullptr;
    renderer::ResourceManager& m_resources;
    std::shared_ptr<renderer::ConstantBufferUpdateBatch> m_packet;
    bool m_enabled = false, m_actorEligible = false;
    size_t m_actorHint = 0;
};
}
