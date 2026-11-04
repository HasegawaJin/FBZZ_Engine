/// @file    TransientGpuState.hpp
/// @brief   GPU 実行状態の所有権をコピーせず、複製先を未初期化から始める共通基底。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Renderer/OwnedResource.hpp>
#include <utility>

namespace fbzz::scene {
/// @note State は所有資源と再確保に必要なキャッシュだけを持ち、設定値は派生 Component に置く。
/// @note コピーは空の状態を作り、移動だけが所有権を渡す。OwnedResource の破棄は各作成元へ返却する。
template<class State>
struct TransientGpuState : State {
    TransientGpuState() = default;
    TransientGpuState(const TransientGpuState&) : State{} { InvalidateDependentState(); }
    TransientGpuState& operator=(const TransientGpuState& other)
    {
        if (this != &other) ResetGpuRuntime();
        return *this;
    }
    TransientGpuState(TransientGpuState&& other) noexcept : State(std::move(other))
    { static_cast<State&>(other) = State{}; }
    TransientGpuState& operator=(TransientGpuState&& other) noexcept
    {
        if (this != &other) {
            static_cast<State&>(*this) = std::move(static_cast<State&>(other));
            static_cast<State&>(other) = State{};
        }
        return *this;
    }
    void ResetGpuRuntime()
    {
        static_cast<State&>(*this) = State{};
        InvalidateDependentState();
    }
private:
    /// @note 空の新規設定と、既存データを再転送するコピー・無効化を区別する。
    void InvalidateDependentState()
    {
        if constexpr (requires(State& state) { state.OnResetGpuRuntime(); })
            static_cast<State&>(*this).OnResetGpuRuntime();
    }
};
} /// @note namespace fbzz::scene
