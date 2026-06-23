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
// 再ビルドするとメンバオフセットが一致しない。各フィールドを個別に保持することで
// ミスマッチ発生時に「何が違うか」をログに出力できる。
// DLL 境界を越えても安全 (両側で同じヘッダを取り込むためレイアウトが一致する)。
struct ScriptDllAbiInfo {
    uint64_t signature;           // 全フィールドの FNV-1a 合成ハッシュ
    uint64_t sizeofScript;
    uint64_t sizeofScene;
    uint64_t sizeofScriptComponent;
    uint64_t componentCount;      // tuple_size_v<ComponentList>
    uint64_t msvcVersion;         // _MSC_VER (0 = 非 MSVC)
    uint64_t iteratorDebugLevel;  // _ITERATOR_DEBUG_LEVEL (0 = 未定義)
};

[[nodiscard]] constexpr ScriptDllAbiInfo GetScriptDllAbiInfo()
{
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME  = 1099511628211ull;

    ScriptDllAbiInfo info{};
    info.sizeofScript          = sizeof(Script);
    info.sizeofScene           = sizeof(Scene);
    info.sizeofScriptComponent = sizeof(ScriptComponent);
    info.componentCount        = std::tuple_size_v<ComponentList>;
#if defined(_MSC_VER)
    info.msvcVersion = _MSC_VER;
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    info.iteratorDebugLevel = _ITERATOR_DEBUG_LEVEL;
#endif

    uint64_t sig = FNV_OFFSET;
    const auto mix = [&](uint64_t v) constexpr { sig ^= v; sig *= FNV_PRIME; };
    mix(info.sizeofScript);
    mix(info.sizeofScene);
    mix(info.sizeofScriptComponent);
    mix(info.componentCount);
    mix(info.msvcVersion);
    mix(info.iteratorDebugLevel);
    info.signature = sig;

    return info;
}

// 署名のみが必要な場合の薄いラッパー。
[[nodiscard]] constexpr uint64_t GetScriptDllAbiSignature()
{
    return GetScriptDllAbiInfo().signature;
}

} // namespace fbzz::scene
