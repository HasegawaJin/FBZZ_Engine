/// @file    SkinningTrace.hpp
/// @brief   Engine と Graphics が1回の Skinning 処理で共有する計測契約。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <cstdint>

#include <Graphics/Api.hpp>

namespace fbzz::renderer::skinning_trace_detail {
/// @note Update fields count void API calls/argument bytes; dispatch successes mean TryDispatch returned true.
struct RequestCounters {
    uint64_t bonePaletteUploadCalls = 0, bonePaletteUploadBytes = 0;
    uint64_t backendRowPoolCreateAttempts = 0, backendRowPoolCreateSuccesses = 0, backendRowPoolReuses = 0;
    uint64_t backendDispatchAttempts = 0, backendDispatchSuccesses = 0, backendDispatchedVertexCount = 0;
};
/// @note Graphics owns the TLS pointer; Engine borrows only stack-owned counters and restores nested callers.
FBZZ_GRAPHICS_API RequestCounters* ExchangeRequestCounters(RequestCounters* counters);

class RequestCounterScope {
public:
    explicit RequestCounterScope(RequestCounters* counters) : m_active(counters != nullptr)
    {
        if (m_active) m_previous = ExchangeRequestCounters(counters);
    }
    ~RequestCounterScope() { if (m_active) ExchangeRequestCounters(m_previous); }
    RequestCounterScope(const RequestCounterScope&) = delete;
    RequestCounterScope& operator=(const RequestCounterScope&) = delete;
private:
    RequestCounters* m_previous = nullptr;
    bool m_active = false;
};
} /// @note namespace fbzz::renderer::skinning_trace_detail
