/// @file    ManagedAiCrowdTransfer.cpp
/// @brief   Atomic one-call crowd packet validation and snapshot publication.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "ManagedAiCrowdTransfer.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <type_traits>

namespace fbzz::scene::managed_detail {
namespace {
constexpr std::uint32_t VERSION = 1, HEADER_BYTES = 48, MAX_PACKET_BYTES = 1024 * 1024;
constexpr std::uint32_t MAX_AGENTS = 4096, MAX_PROFILES = 64, MAX_THREATS = 64, MAX_OBSTACLES = 256;
static_assert(std::endian::native == std::endian::little);
static_assert(std::is_trivially_copyable_v<ai::CrowdSettings> && std::is_trivially_copyable_v<ai::CrowdProfile>
    && std::is_trivially_copyable_v<ai::CrowdAgent> && std::is_trivially_copyable_v<ai::CrowdThreat>
    && std::is_trivially_copyable_v<ai::CrowdObstacle> && std::is_trivially_copyable_v<ai::CrowdStats>);

template<typename T>
void Read(const char* bytes, std::size_t& offset, std::uint32_t count, std::vector<T>& values)
{
    values.resize(count);
    if (count) std::memcpy(values.data(), bytes + offset, count * sizeof(T));
    offset += count * sizeof(T);
}
}

ManagedBridgeStatus ManagedAiCrowdTransfer::Run(const ManagedBridgeRequest& request, ManagedBridgeReply& reply)
{
    FBZZ_PROFILE_SCOPE("LateScriptSystem::NativeCrowdTransfer");
    reply.index = 0; reply.flags = 0; reply.outputBytes = 0; reply.reserved = 0;
    if (m_inFlight) return ManagedBridgeStatus::INVALID_SCOPE;
    if (request.flags || request.index || request.integer || request.reserved0 || request.reserved1
        || request.x != 0 || request.y != 0 || request.z != 0 || request.w != 0
        || !request.input || request.inputBytes < HEADER_BYTES || request.inputBytes > MAX_PACKET_BYTES
        || request.outputCapacity > MAX_PACKET_BYTES || (request.outputCapacity && !request.output))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    std::array<std::uint32_t, 12> header{};
    std::memcpy(header.data(), request.input, HEADER_BYTES);
    if (header[0] != VERSION || header[1] != sizeof(ai::CrowdSettings) || header[2] != sizeof(ai::CrowdProfile)
        || header[3] != sizeof(ai::CrowdAgent) || header[4] != sizeof(ai::CrowdThreat)
        || header[5] != sizeof(ai::CrowdObstacle) || header[6] != sizeof(ai::CrowdStats) || header[11])
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto agents = header[7], profiles = header[8], threats = header[9], obstacles = header[10];
    if (agents > MAX_AGENTS || !profiles || profiles > MAX_PROFILES || threats > MAX_THREATS || obstacles > MAX_OBSTACLES)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const std::size_t inputBytes = HEADER_BYTES + sizeof(ai::CrowdSettings) + profiles * sizeof(ai::CrowdProfile)
        + agents * sizeof(ai::CrowdAgent) + threats * sizeof(ai::CrowdThreat) + obstacles * sizeof(ai::CrowdObstacle);
    const std::size_t outputBytes = sizeof(ai::CrowdStats) + agents * sizeof(ai::CrowdAgent);
    if (inputBytes != request.inputBytes) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (request.outputCapacity < outputBytes || !request.output) {
        reply.outputBytes = static_cast<std::uint32_t>(outputBytes);
        return ManagedBridgeStatus::BUFFER_TOO_SMALL;
    }
    /// @note Overlapping packet ranges are rejected; an error cannot overwrite the caller's input snapshot.
    const std::less<const char*> before;
    if (before(request.input, request.output + outputBytes) && before(request.output, request.input + inputBytes))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    m_inFlight = true;
    struct FlightGuard { bool& flight; ~FlightGuard() { flight = false; } } guard{m_inFlight};
    std::size_t offset = HEADER_BYTES;
    ai::CrowdSettings settings{};
    std::memcpy(&settings, request.input + offset, sizeof(settings)); offset += sizeof(settings);
    Read(request.input, offset, profiles, m_profiles); Read(request.input, offset, agents, m_agents);
    Read(request.input, offset, threats, m_threats); Read(request.input, offset, obstacles, m_obstacles);
    m_output.resize(agents);
    ai::CrowdStats stats{};
    {
        FBZZ_PROFILE_SCOPE("LateScriptSystem::NativeCrowdStep");
        if (!ai::StepCrowd(settings, m_profiles, m_agents, m_threats, m_obstacles, m_output, stats, m_workspace))
            return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    std::memcpy(request.output, &stats, sizeof(stats));
    if (agents) std::memcpy(request.output + sizeof(stats), m_output.data(), agents * sizeof(ai::CrowdAgent));
    reply.index = agents; reply.outputBytes = static_cast<std::uint32_t>(outputBytes);
    return ManagedBridgeStatus::OK;
}
}
