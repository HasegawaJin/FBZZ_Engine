/// @file    ManagedMaterialTransfers.cpp
/// @brief   float64 packet・explicit slot/birth/call fence と shader 型検証。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedMaterialTransfers.hpp"
#include "ManagedPacketRead.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
namespace fbzz::scene::managed_detail {
namespace {
template<typename T> bool Read(std::string_view bytes, size_t& position, T& value)
{
    return ReadManagedValue(bytes, position, value);
}
}
struct ManagedMaterialTransfers::Impl {
    struct Record {
        uint64_t owner, instance, generation, call, scene, birth;
        EntityID entity; uint32_t slot; bool read; uint64_t expected;
        std::string property, material, shader;
        std::vector<double> values;
        bool captured = false;
        MaterialSlot::AuthoringSnapshotPtr slotOwner;
    };
    /// @note Reacquire by stable entity/birth after importer/reflection callbacks; no old GameObject/component/slot borrow is used.
    static ManagedBridgeStatus ValidateOwner(Scene& scene, const Record& record, MaterialComponent*& material, bool opening = false)
    {
        if (record.scene != scene.GetRenderSceneGeneration()) return ManagedBridgeStatus::INVALID_SCOPE;
        auto* target = scene.GetGameObject(record.entity);
        material = target ? target->GetComponent<MaterialComponent>() : nullptr;
        if (!material || ManagedObjectControls::IsPendingComponentRemoval(*target, 29)
            || scene.GetComponentLifetime<MaterialComponent>(record.entity) != record.birth || record.slot >= material->SlotCount())
            return ManagedBridgeStatus::NOT_FOUND;
        const auto snapshot = material->RawSlotAt(record.slot).ReadAuthoring();
        if (!snapshot || !record.slotOwner || snapshot->OwnerToken() != record.slotOwner->OwnerToken()
            || snapshot->Data().materialPath != record.material) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto& row = material->RawSlotAt(record.slot);
        if (!opening && (row.GetMaterialAsset().IsValid() ? row.GetShaderPath() : std::string{}) != record.shader)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        return ManagedBridgeStatus::OK;
    }
    uint64_t next = 1; std::unordered_map<uint64_t, Record> records;
};
ManagedMaterialTransfers::ManagedMaterialTransfers() : m_impl(std::make_unique<Impl>()) {}
ManagedMaterialTransfers::~ManagedMaterialTransfers() = default;
ManagedBridgeStatus ManagedMaterialTransfers::Open(Scene& scene, GameObject& target, Script& proxy,
    const ManagedBridgeRequest& request, int64_t slot, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result)
{
    const bool read = request.index == 30;
    if (request.flags != (read ? 1U : 0U) || !request.callId || !request.instanceId || !request.ownerId || !request.generation
        || !values.empty() || text.size() != (read ? 1U : 2U) || text[0].empty() || text[0].find('\0') != std::string::npos) return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto* material = target.GetComponent<MaterialComponent>();
    if (!material || ManagedObjectControls::IsPendingComponentRemoval(target, 29)) return ManagedBridgeStatus::NOT_FOUND;
    if (slot < 0 || static_cast<uint64_t>(slot) >= material->SlotCount()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (m_impl->records.size() >= 32 || m_impl->next > static_cast<uint64_t>(INT64_MAX)) return ManagedBridgeStatus::PENDING_LIMIT;
    const auto slotSnapshot = material->RawSlotAt(static_cast<size_t>(slot)).ReadAuthoring();
    Impl::Record record{request.ownerId, request.instanceId, request.generation, request.callId, scene.GetRenderSceneGeneration(),
        scene.GetComponentLifetime<MaterialComponent>(target.GetID()), target.GetID(), static_cast<uint32_t>(slot), read, 0,
        text[0], slotSnapshot->Data().materialPath,
        material->RawSlotAt(static_cast<size_t>(slot)).GetMaterialAsset().IsValid()
            ? material->RawSlotAt(static_cast<size_t>(slot)).GetShaderPath() : std::string{}, {}};
    record.slotOwner = slotSnapshot;
    if (read) {
        size_t count = 0;
        const bool available = proxy.material.Instance(record.slot).TryGetValueCount(MaterialPropertyId(record.property), count);
        const auto status = Impl::ValidateOwner(scene, record, material, true);
        if (status != ManagedBridgeStatus::OK) return status;
        if (!available) {
            result = {}; return ManagedBridgeStatus::OK;
        }
        record.expected = count;
        const auto& row = material->RawSlotAt(record.slot);
        record.shader = row.GetMaterialAsset().IsValid() ? row.GetShaderPath() : std::string{};
    } else {
        const auto parsed = std::from_chars(text[1].data(), text[1].data() + text[1].size(), record.expected);
        if (parsed.ec != std::errc{} || parsed.ptr != text[1].data() + text[1].size() || record.expected > record.values.max_size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        /// @note 初回の遅延ロードを commit の変更検査と混同しない。ロード callback 後は旧 Component / slot を再利用しない。
        (void)material->EnsureRawMaterialAsset(record.slot);
        const auto status = Impl::ValidateOwner(scene, record, material, true);
        if (status != ManagedBridgeStatus::OK) return status;
        const auto& row = material->RawSlotAt(record.slot);
        record.shader = row.GetMaterialAsset().IsValid() ? row.GetShaderPath() : std::string{};
        record.values.reserve(static_cast<size_t>(record.expected));
    }
    if (m_impl->records.size() >= 32 || m_impl->next > static_cast<uint64_t>(INT64_MAX)) return ManagedBridgeStatus::PENDING_LIMIT;
    const auto token = m_impl->next++; const auto count = record.expected;
    m_impl->records.emplace(token, std::move(record)); result = {}; result.integer = static_cast<int64_t>(token); result.text = {std::to_string(count)};
    return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus ManagedMaterialTransfers::Run(Scene& scene, GameObject& target, Script& proxy,
    const ManagedBridgeRequest& request, ManagedServiceResult& result, ManagedBridgeReply* rawReply)
{
    if (!request.input || request.inputBytes < 8) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const std::string_view bytes(request.input, request.inputBytes); size_t position = 0; uint64_t token = 0;
    if (!Read(bytes, position, token)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto found = m_impl->records.find(token); if (found == m_impl->records.end()) return ManagedBridgeStatus::NOT_FOUND;
    auto& record = found->second;
    if (record.owner != request.ownerId || record.instance != request.instanceId || record.generation != request.generation || record.call != request.callId
        || record.scene != scene.GetRenderSceneGeneration() || record.entity != target.GetID() || request.flags != (record.read ? 1U : 0U)) return ManagedBridgeStatus::INVALID_SCOPE;
    MaterialComponent* material = nullptr;
    const auto ownerStatus = Impl::ValidateOwner(scene, record, material);
    if (ownerStatus != ManagedBridgeStatus::OK) { m_impl->records.erase(found); return ownerStatus; }
    if (request.index == 35) { if (bytes.size() != 8) return ManagedBridgeStatus::INVALID_ARGUMENT; m_impl->records.erase(found); result = {}; return ManagedBridgeStatus::OK; }
    if (request.index == 31 || request.index == 33) {
        uint64_t offset = 0; uint32_t count = 0, reserved = 0;
        if (!Read(bytes, position, offset) || !Read(bytes, position, count) || !Read(bytes, position, reserved) || reserved
            || count > (65536U - 24U) / sizeof(double) || offset > record.expected || count > record.expected - offset) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 31) {
            if (!record.read || bytes.size() != 24 || !rawReply || (count && !request.output)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            const uint32_t needed = count * static_cast<uint32_t>(sizeof(double));
            if (request.outputCapacity < needed) { rawReply->outputBytes = needed; return ManagedBridgeStatus::BUFFER_TOO_SMALL; }
            /// @note Own reflection inputs across native callbacks; count-only opens still defer conversion until the first page.
            if (needed && !record.captured) {
                const auto ownedRecord = record;
                std::vector<double> capturedValues;
                const bool available = proxy.material.Instance(ownedRecord.slot).TryGetValues(MaterialPropertyId(ownedRecord.property), capturedValues);
                const auto current = m_impl->records.find(token);
                if (current == m_impl->records.end()) return ManagedBridgeStatus::NOT_FOUND;
                const auto status = Impl::ValidateOwner(scene, ownedRecord, material);
                if (status != ManagedBridgeStatus::OK) { m_impl->records.erase(current); return status; }
                if (!available || capturedValues.size() != ownedRecord.expected) {
                    m_impl->records.erase(current); return ManagedBridgeStatus::INVALID_ARGUMENT;
                }
                auto& freshRecord = current->second;
                if (!freshRecord.captured) { freshRecord.values = std::move(capturedValues); freshRecord.captured = true; }
                std::memcpy(request.output, freshRecord.values.data() + static_cast<size_t>(offset), needed);
                rawReply->outputBytes = needed; rawReply->integer = static_cast<int64_t>(count); rawReply->callId = freshRecord.scene;
                return ManagedBridgeStatus::OK;
            }
            if (needed) std::memcpy(request.output, record.values.data() + static_cast<size_t>(offset), needed);
            rawReply->outputBytes = needed; rawReply->integer = static_cast<int64_t>(count); rawReply->callId = record.scene;
            return ManagedBridgeStatus::OK;
        }
        if (record.read || !count || offset != record.values.size() || bytes.size() - position != count * sizeof(double)) { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
        const size_t start = position;
        for (uint32_t index = 0; index < count; ++index) {
            double value = 0; if (!Read(bytes, position, value) || !std::isfinite(value)) { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
        }
        position = start;
        for (uint32_t index = 0; index < count; ++index) { double value = 0; (void)Read(bytes, position, value); record.values.push_back(value); }
        result = {}; result.integer = static_cast<int64_t>(record.values.size()); return ManagedBridgeStatus::OK;
    }
    if (request.index != 34 || record.read || bytes.size() != 8 || record.values.size() != record.expected) { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
    const auto ownedRecord = record;
    const bool written = proxy.material.Instance(ownedRecord.slot).SetValues(MaterialPropertyId(ownedRecord.property), ownedRecord.values);
    const auto current = m_impl->records.find(token);
    if (current == m_impl->records.end()) return ManagedBridgeStatus::NOT_FOUND;
    const auto status = Impl::ValidateOwner(scene, ownedRecord, material);
    m_impl->records.erase(current);
    if (status != ManagedBridgeStatus::OK) return status;
    result = {}; result.integer = written; return ManagedBridgeStatus::OK;
}
void ManagedMaterialTransfers::CancelOwner(uint64_t owner) { std::erase_if(m_impl->records, [owner](const auto& pair) { return pair.second.owner == owner; }); }
void ManagedMaterialTransfers::RetireGeneration(uint64_t generation) { std::erase_if(m_impl->records, [generation](const auto& pair) { return pair.second.generation == generation; }); }
void ManagedMaterialTransfers::EndCall(uint64_t call) { std::erase_if(m_impl->records, [call](const auto& pair) { return pair.second.call == call; }); }
} /// @note namespace fbzz::scene::managed_detail
