/// @file    ManagedInputBridge.cpp
/// @brief   登録済みの無入力と未知の入力名を区別し、InputActionMap を唯一の正本にする。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include "ManagedInputBridge.hpp"
#include <Engine/Input/InputActionMap.hpp>
#include <array>
#include <bit>
#include <limits>
#include <utility>

namespace fbzz::scene::managed_detail {
namespace {

static_assert(std::endian::native == std::endian::little);

void AppendCount(std::string& bytes, uint32_t count)
{
    const auto encoded = std::bit_cast<std::array<char, sizeof(count)>>(count);
    bytes.append(encoded.data(), encoded.size());
}

template<typename T>
ManagedBridgeStatus Names(const std::vector<T>& entries, ManagedBridgeReply& reply, std::string& names)
{
    if (entries.size() > std::numeric_limits<uint32_t>::max()) return ManagedBridgeStatus::CAPACITY_LIMIT;
    std::string encoded;
    AppendCount(encoded, static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        if (entry.name.size() > std::numeric_limits<uint32_t>::max())
            return ManagedBridgeStatus::CAPACITY_LIMIT;
        AppendCount(encoded, static_cast<uint32_t>(entry.name.size()));
        encoded.append(entry.name);
    }
    reply.valueKind = ManagedValueKind::STRING_ARRAY;
    reply.index = static_cast<uint32_t>(entries.size());
    names = std::move(encoded);
    return ManagedBridgeStatus::OK;
}

} /// @note namespace

ManagedBridgeStatus ManagedInputBridge::Read(std::string_view name, uint32_t state, ManagedBridgeReply& reply)
{
    if (name.empty() || state > static_cast<uint32_t>(ManagedInputValue::AXIS)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool axis = state == static_cast<uint32_t>(ManagedInputValue::AXIS);
    if (axis ? !input::InputActionMap::FindAxis(name) : !input::InputActionMap::FindAction(name))
        return ManagedBridgeStatus::NOT_FOUND;
    if (axis) {
        reply.valueKind = ManagedValueKind::FLOAT;
        reply.x = input::InputActionMap::GetAxis(name);
    } else {
        reply.valueKind = ManagedValueKind::BOOL;
        reply.integer = state == static_cast<uint32_t>(ManagedInputValue::HELD) ? input::InputActionMap::GetAction(name)
            : state == static_cast<uint32_t>(ManagedInputValue::DOWN) ? input::InputActionMap::GetActionDown(name)
            : input::InputActionMap::GetActionUp(name);
    }
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedInputBridge::Query(std::string_view name, uint32_t query, ManagedBridgeReply& reply, std::string& names)
{
    if (query > static_cast<uint32_t>(ManagedInputQuery::AXIS_NAMES)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool existence = query <= static_cast<uint32_t>(ManagedInputQuery::AXIS_EXISTS);
    if (existence ? name.empty() : !name.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (existence) {
        reply.valueKind = ManagedValueKind::BOOL;
        reply.integer = query == static_cast<uint32_t>(ManagedInputQuery::AXIS_EXISTS)
            ? input::InputActionMap::FindAxis(name) != nullptr : input::InputActionMap::FindAction(name) != nullptr;
        return ManagedBridgeStatus::OK;
    }
    return query == static_cast<uint32_t>(ManagedInputQuery::AXIS_NAMES)
        ? Names(input::InputActionMap::GetAxes(), reply, names) : Names(input::InputActionMap::GetActions(), reply, names);
}

} /// @note namespace fbzz::scene::managed_detail
