// FBZZ Engine
// Entity.hpp | fbzz::scene
// Scene 内の GameObject を識別する ID
// index と generation を組み合わせ、破棄済み参照を検出する。
// 外部公開は GameObject 経由を基本とし、System 内部で軽量参照として使う。
#pragma once
#include <cstdint>

namespace fbzz::scene {

struct EntityID {
    static constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;

    uint32_t index      = INVALID_INDEX;
    uint32_t generation = 0;

    bool IsValid() const { return index != INVALID_INDEX; }
    bool operator==(const EntityID&) const = default;

    static const EntityID INVALID;
};

inline const EntityID EntityID::INVALID = {};

// if constexpr の else 節で使う依存 false
template<typename T>
inline constexpr bool AlwaysFalse = false;

} // namespace fbzz::scene
