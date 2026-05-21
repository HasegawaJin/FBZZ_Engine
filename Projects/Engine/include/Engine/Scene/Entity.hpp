// FBZZ Engine
// Entity.hpp | fbzz::scene
// Entity の識別子。generation カウンタで破棄済み参照を検出する
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

// if constexpr の else 節で使う dependent false
template<typename T>
inline constexpr bool AlwaysFalse = false;

} // namespace fbzz::scene
