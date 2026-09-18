/// @file    TypeIndex.hpp
/// @brief   RTTI に依存しないコンパイル時型 ID。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// @note ComponentArray や登録表で型ごとの連番 ID が必要な場合に使う。
///       dynamic_cast の代替ではなく、型別ストレージのキーとして使う。
#pragma once
#include <cstdint>

namespace fbzz::util {

class TypeIndex {
public:
    /// @brief T ごとに一意な uint32_t を返す。初回呼び出し時に採番される。
    template<typename T>
    static uint32_t Get() {
        static const uint32_t id = s_counter++;
        return id;
    }

private:
    static inline uint32_t s_counter = 0;
};

} // namespace fbzz::util
