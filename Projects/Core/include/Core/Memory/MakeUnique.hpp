/// @file    MakeUnique.hpp
/// @brief   例外を投げない unique_ptr の生成。
/// @author  Hasegawa Jin
/// @date    2026-09-02
/// @note 本エンジンは throw/std::exception を使わない規約で、std::make_unique は確保失敗時に std::bad_alloc を投げ規約の外になる (受け取れないまま std::terminate) ため使わない。«失敗したら nullptr» に揃え、呼び出し側が普段の «作れなかった» と同じ形で扱えるようにする。
/// @note 素の `new` を書いてよいのはこのファイルだけ。ほかは MakeUnique / MakeUniqueArray を使う。
#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace fbzz::core {

/// @note 失敗したら nullptr を返す make_unique。
template <class T, class... Args>
[[nodiscard]] std::unique_ptr<T> MakeUnique(Args&&... args)
{
    return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

/// @note 配列版。要素は値初期化される。
template <class T>
[[nodiscard]] std::unique_ptr<T[]> MakeUniqueArray(std::size_t count)
{
    return std::unique_ptr<T[]>(new (std::nothrow) T[count]());
}

} /// @note namespace fbzz::core
