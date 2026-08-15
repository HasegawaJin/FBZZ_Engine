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

#ifndef FBZZ_ENGINE_VERSION_MAJOR
#define FBZZ_ENGINE_VERSION_MAJOR 0
#define FBZZ_ENGINE_VERSION_MINOR 0
#define FBZZ_ENGINE_VERSION_PATCH 0
#define FBZZ_ENGINE_VERSION_STRING "0.0.0-unconfigured"
#endif
#ifndef FBZZ_BUILD_CONFIG_ID
#define FBZZ_BUILD_CONFIG_ID 0
#define FBZZ_BUILD_CONFIG_NAME "Unconfigured"
#endif

#if defined(_MSC_VER)
// WHY: 同じヘッダーを使っていても Engine 版数・構成が違う Script DLL は C++ ABI 非互換である。
//      MSVC linker の detect_mismatch で混在をリンク時にも拒否し、実行時署名を最後の防壁にする。
#define FBZZ_ABI_STRINGIZE_DETAIL(value) #value
#define FBZZ_ABI_STRINGIZE(value) FBZZ_ABI_STRINGIZE_DETAIL(value)
#pragma detect_mismatch("FBZZ.EngineVersion", FBZZ_ENGINE_VERSION_STRING)
#pragma detect_mismatch("FBZZ.BuildConfig", FBZZ_BUILD_CONFIG_NAME)
#pragma detect_mismatch("FBZZ.MsvcFullVersion", FBZZ_ABI_STRINGIZE(_MSC_FULL_VER))
#if defined(_WIN64)
#pragma detect_mismatch("FBZZ.Architecture", "x64")
#else
#pragma detect_mismatch("FBZZ.Architecture", "unsupported")
#endif
#undef FBZZ_ABI_STRINGIZE
#undef FBZZ_ABI_STRINGIZE_DETAIL
#endif

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
    uint64_t msvcFullVersion;     // _MSC_FULL_VER (0 = 非 MSVC)
    uint64_t iteratorDebugLevel;  // _ITERATOR_DEBUG_LEVEL (0 = 未定義)
    uint64_t engineVersion;       // major/minor/patch を 64 bit に pack
    uint64_t buildConfiguration;  // 1=Debug, 2=Development, 3=Release
    uint64_t pointerSize;         // x64 SDK は 8 のみ許可
    uint64_t dynamicRuntime;      // /MD または /MDd なら 1
};

// リフレクション ABI バージョン。
// WHY: IReflector の仮想関数の並び (Field / RefField / Tooltip / IntRange / FloatRange / Enum 等) は
//      sizeof では捉えられないが、スクリプト DLL の Reflect() は vtable インデックスで仮想呼び出しする。
//      並びを変えたのに DLL が再ビルドされていないと、署名が一致したまま vtable がズレてクラッシュする。
//      IReflector の仮想関数を追加・削除・並べ替えたら必ずこの値をインクリメントすること。
//      これにより stale な DLL は署名不一致で安全に拒否される (Missing Script 表示、クラッシュしない)。
// 履歴:
//   3: BeginObject / EndObject / BeginObjectList / BeginObjectElement /
//      EndObjectElement / EndObjectList を IReflector 末尾へ追加
//      (入れ子オブジェクトと構造体配列のリフレクション対応)
constexpr uint64_t kReflectionAbiVersion = 3;

// Script 仮想関数テーブルの世代。
// WHY: kReflectionAbiVersion と同じ問題が Script 本体にもある。OnUpdate / OnCollisionEnter /
//      Reflect などはすべて vtable インデックスで呼ばれるが、仮想関数を途中に挿入しても
//      sizeof(Script) は変わらないため、既存の署名では stale な DLL を検出できない。
//      Script の仮想関数を追加・削除・並べ替えたら必ずこの値をインクリメントすること。
// 履歴:
//   1: 初版 (この定数の導入時点の並び)
//   2: OnAnimationEvent の直後へ OnAnimatorMove を追加 (Root Motion の同期コールバック)
constexpr uint64_t kScriptVtableAbiVersion = 2;

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
    info.msvcFullVersion = _MSC_FULL_VER;
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    info.iteratorDebugLevel = _ITERATOR_DEBUG_LEVEL;
#endif
    info.engineVersion = (static_cast<uint64_t>(FBZZ_ENGINE_VERSION_MAJOR) << 40)
        | (static_cast<uint64_t>(FBZZ_ENGINE_VERSION_MINOR) << 20)
        | static_cast<uint64_t>(FBZZ_ENGINE_VERSION_PATCH);
    info.buildConfiguration = FBZZ_BUILD_CONFIG_ID;
    info.pointerSize = sizeof(void*);
#if defined(_DLL)
    info.dynamicRuntime = 1;
#endif

    uint64_t sig = FNV_OFFSET;
    const auto mix = [&](uint64_t v) constexpr { sig ^= v; sig *= FNV_PRIME; };
    mix(info.sizeofScript);
    mix(info.sizeofScene);
    mix(info.sizeofScriptComponent);
    mix(info.componentCount);
    mix(info.msvcVersion);
    mix(info.msvcFullVersion);
    mix(info.iteratorDebugLevel);
    mix(info.engineVersion);
    mix(info.buildConfiguration);
    mix(info.pointerSize);
    mix(info.dynamicRuntime);
    mix(kReflectionAbiVersion);     // IReflector vtable レイアウトの世代を署名へ反映
    mix(kScriptVtableAbiVersion);   // Script vtable レイアウトの世代を署名へ反映
    info.signature = sig;

    return info;
}

// 署名のみが必要な場合の薄いラッパー。
[[nodiscard]] constexpr uint64_t GetScriptDllAbiSignature()
{
    return GetScriptDllAbiInfo().signature;
}

} // namespace fbzz::scene
