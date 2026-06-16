// FBZZ Engine
// ScriptDllAbi.hpp | fbzz::scene
// Script DLL とホスト実行ファイルの C++ ABI 互換性を検証する署名
#pragma once

#include "ComponentRegistry.hpp"
#include "Scene.hpp"
#include "Script.hpp"
#include "ScriptComponent.hpp"
#include <cstdint>
#include <tuple>

namespace fbzz::scene {

// Script DLL は Scene / Script の具象 C++ 型を共有するため、どちらか一方だけを
// 再ビルドするとメンバオフセットが一致しない。型レイアウトを署名化し、
// 古い DLL をロード時に拒否してアクセス違反を防ぐ。
[[nodiscard]] constexpr uint64_t GetScriptDllAbiSignature()
{
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;

    uint64_t signature = FNV_OFFSET;
    const auto mix = [&](uint64_t value) constexpr {
        signature ^= value;
        signature *= FNV_PRIME;
    };

    mix(sizeof(Scene));
    mix(alignof(Scene));
    mix(sizeof(Script));
    mix(alignof(Script));
    mix(sizeof(ScriptComponent));
    mix(alignof(ScriptComponent));
    mix(std::tuple_size_v<ComponentList>);
#if defined(_MSC_VER)
    mix(_MSC_VER);
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    mix(_ITERATOR_DEBUG_LEVEL);
#endif
    return signature;
}

} // namespace fbzz::scene
