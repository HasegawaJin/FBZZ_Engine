/// @file    MakeUnique.hpp
/// @brief   例外を投げない unique_ptr の生成。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// WHY std::make_unique を使わないか:
///   本エンジンは throw / std::exception を使わない規約で、`/EHsc` の下でも受け手が居ない。
///   std::make_unique は確保に失敗すると std::bad_alloc を投げるため、そこだけ規約の外に
///   なる (受け取れないまま std::terminate)。«失敗したら nullptr» に揃えて、
///   呼び出し側が普段の «作れなかった» と同じ形で扱えるようにする。
///
/// NOTE: 素の `new` を書いてよいのはこのファイルだけ。ほかは MakeUnique / MakeUniqueArray を使う。
#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace fbzz::core {

/// 失敗したら nullptr を返す make_unique。
template <class T, class... Args>
[[nodiscard]] std::unique_ptr<T> MakeUnique(Args&&... args)
{
    return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

/// 配列版。要素は値初期化される。
template <class T>
[[nodiscard]] std::unique_ptr<T[]> MakeUniqueArray(std::size_t count)
{
    return std::unique_ptr<T[]>(new (std::nothrow) T[count]());
}

} // namespace fbzz::core
