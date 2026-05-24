// FBZZ Engine
// TypeIndex.hpp | fbzz::util
// RTTI に依存しないコンパイル時型ID (ヘッダーオンリー)
#pragma once
#include <cstdint>

namespace fbzz::util {

class TypeIndex {
public:
    // T ごとに一意な uint32_t を返す。初回呼び出し時に採番される
    template<typename T>
    static uint32_t Get() {
        static const uint32_t id = s_counter++;
        return id;
    }

private:
    static inline uint32_t s_counter = 0;
};

} // namespace fbzz::util
