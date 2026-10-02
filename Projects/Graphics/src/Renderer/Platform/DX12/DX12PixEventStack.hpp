/// @file    DX12PixEventStack.hpp
/// @brief   Command-list-local PIX scopes retained across successful list resets.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::renderer {

/// @note Emitters receive (begin, color, name); stale tokens never close a newer recording.
/// @note Suspend balances the SDK's implicit CPU events as well as its command-list GPU events.
/// @see https://devblogs.microsoft.com/pix/winpixeventruntime/ CPU events implicit in GPU event overloads.
class DX12PixEventStack final {
public:
    template<class Emit>
    void Resume(Emit&& emit)
    {
        if (m_recording) return;
        m_recording = true;
        for (const auto& event : m_events) emit(true, event.color, event.name);
    }

    template<class Emit>
    void Suspend(Emit&& emit)
    {
        if (!m_recording) return;
        for (auto i = m_events.size(); i != 0; --i)
            emit(false, m_events[i - 1].color, m_events[i - 1].name);
        m_recording = false;
    }

    template<class Emit>
    void Discard(Emit&& emit)
    {
        Suspend(emit);
        m_events.clear();
    }

    template<class Emit>
    [[nodiscard]] uint64_t Begin(std::string_view name, uint64_t color, Emit&& emit)
    {
        if (!m_recording || m_nextToken == 0) return 0;
        const uint64_t token = m_nextToken++;
        m_events.push_back({std::string(name), color, token});
        emit(true, color, m_events.back().name);
        return token;
    }

    /// @return False for out-of-order, retired, or absent tokens; the remaining stack is unchanged.
    template<class Emit>
    bool End(uint64_t token, Emit&& emit)
    {
        if (token == 0 || m_events.empty() || m_events.back().token != token) return false;
        if (m_recording) emit(false, m_events.back().color, m_events.back().name);
        m_events.pop_back();
        return true;
    }

    [[nodiscard]] bool IsRecording() const { return m_recording; }
    [[nodiscard]] std::size_t GetDepth() const { return m_events.size(); }

private:
    struct Event {
        std::string name;
        uint64_t color = 0;
        uint64_t token = 0;
    };
    std::vector<Event> m_events;
    uint64_t m_nextToken = 1;
    bool m_recording = false;
};

} /// @note namespace fbzz::renderer
