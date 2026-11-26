/// @file    ProfilerLegacyScopes.cpp
/// @brief   互換 BeginSample / EndSample の対応と中断時の復元。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "ProfilerThreadState.hpp"

namespace fbzz::profiler {

void Profiler::BeginSample(const ProfilerMarker& marker)
{
    auto& state = detail::CurrentThreadState();
    auto& legacyScopes = state.m_legacyScopes;

    // 上限を初めて超える直前の位置を保存し、抑制した区間をすべて閉じたときに戻せるようにする。
    const bool legacyStackIsFull = legacyScopes.m_tokens.size() == ProfileRecorder::MAX_DEPTH;
    const bool startsSuppression = legacyStackIsFull && legacyScopes.m_suppressedDepth == 0;
    if (startsSuppression) {
        legacyScopes.m_overflowCheckpoint = state.m_recorder.Checkpoint();
    }
    const auto token = BeginScope(marker);
    if (!token.recorderId) {
        return;
    }

    if (startsSuppression) {
        legacyScopes.m_overflowRootScopeId = token.scopeId;
    }
    const bool suppressLegacyScope = legacyScopes.m_tokens.size() == ProfileRecorder::MAX_DEPTH
        || legacyScopes.m_suppressedDepth != 0;
    if (suppressLegacyScope) {
        ++legacyScopes.m_suppressedDepth;
        return;
    }
    legacyScopes.m_tokens.push_back(token);
}

void Profiler::EndSample()
{
    auto& state = detail::CurrentThreadState();
    auto& legacyScopes = state.m_legacyScopes;
    if (legacyScopes.m_suppressedDepth) {
        --legacyScopes.m_suppressedDepth;
        if (legacyScopes.m_suppressedDepth == 0) {
            state.m_recorder.Recover(legacyScopes.m_overflowCheckpoint);
            legacyScopes.m_overflowCheckpoint = {};
            legacyScopes.m_overflowRootScopeId = 0;
        }
        return;
    }

    if (legacyScopes.m_tokens.empty()) {
        return;
    }
    const auto token = legacyScopes.m_tokens.back();
    legacyScopes.m_tokens.pop_back();
    EndScope(token);
}

ProfilerCheckpoint Profiler::Checkpoint()
{
    auto& state = detail::CurrentThreadState();
    auto& legacyScopes = state.m_legacyScopes;
    return {state.m_recorder.Checkpoint(), legacyScopes.m_overflowCheckpoint,
            legacyScopes.m_tokens.empty() ? 0 : legacyScopes.m_tokens.back().scopeId, legacyScopes.m_overflowRootScopeId,
            static_cast<uint32_t>(legacyScopes.m_tokens.size()), legacyScopes.m_suppressedDepth};
}

void Profiler::Recover(ProfilerCheckpoint checkpoint)
{
    auto& state = detail::CurrentThreadState();
    auto& legacyScopes = state.m_legacyScopes;
    if (checkpoint.compatStackSize > legacyScopes.m_tokens.size()) {
        return;
    }
    if (checkpoint.compatSuppressedDepth > legacyScopes.m_suppressedDepth) {
        return;
    }
    if (checkpoint.compatStackSize && legacyScopes.m_tokens[checkpoint.compatStackSize - 1].scopeId != checkpoint.compatTopScopeId) {
        return;
    }
    // 抑制区間の ID も照合し、すでに閉じた同じ深さの区間への復元を拒否する。
    if (checkpoint.compatSuppressedDepth && checkpoint.compatOverflowScopeId != legacyScopes.m_overflowRootScopeId) {
        return;
    }
    if (!state.m_recorder.Recover(checkpoint.recorder)) {
        return;
    }

    // 計測器が復元を受理してから、旧 API 側のスタックも同じ位置へ戻す。
    legacyScopes.m_tokens.resize(checkpoint.compatStackSize);
    legacyScopes.m_suppressedDepth = checkpoint.compatSuppressedDepth;
    legacyScopes.m_overflowCheckpoint = checkpoint.compatOverflow;
    legacyScopes.m_overflowRootScopeId = checkpoint.compatOverflowScopeId;
}

}
