/// @file    SandboxScriptsDll.cpp
/// @brief   スクリプト DLL のエントリポイント。
/// @author  Hasegawa Jin
/// @date    2026-06-03
///
/// @note fbzz_engine は EXE / Script DLL が共有する runtime。Script DLL は任意のユーザーコードを読み込む拡張境界なので、
///       登録 API は DLL からグローバル状態へ直接アクセスさせず EXE が渡す関数ポインタ経由にし、ScriptFactory の所有権をホスト側に固定する。

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
#include "Scripts/TpsCameraComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

/// @note ScriptSceneProxy::GetComponent<T>() の定義は Scene.hpp 末尾にある。スクリプトヘッダーは Script.hpp しか
///       include しないため、DLL エントリポイントで Scene.hpp を明示 include して全特殊化をこの TU でインスタンス化する。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptDllAbi.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/// @name DLL API マクロ
#ifdef SANDBOXSCRIPTS_EXPORTS
#  define SANDBOXSCRIPTS_API __declspec(dllexport)
#else
#  define SANDBOXSCRIPTS_API __declspec(dllimport)
#endif

using ScriptRegisterFn = std::function<void(
    const std::string&,
    std::function<std::unique_ptr<fbzz::scene::Script>()>
)>;

namespace {

struct ScriptEntry {
    std::string name;
    std::function<std::unique_ptr<fbzz::scene::Script>()> factory;
};

/// @note DLL 内に登録済みスクリプト一覧を保持し、SandboxScripts_Register() を複数回呼んでも正しく再登録できるようにする。
/// @note エントリは Scripts/ScriptList.inl で一元管理する。ScriptCodeGen は ScriptList.inl だけを更新するため、このファイルのエントリは手動編集不要。
const std::vector<ScriptEntry>& AllEntries()
{
    static const std::vector<ScriptEntry> entries = {
#define FBZZ_SCRIPT_ENTRY(ns, T) \
        { ::ns::T::TYPE_NAME, []() { return std::make_unique<::ns::T>(); } },
#include "Scripts/ScriptList.inl"
#undef FBZZ_SCRIPT_ENTRY
    };
    return entries;
}

} // namespace

extern "C" {

/// @brief ホストと DLL の型レイアウトが一致する場合だけ ScriptFactory 登録を許可する。
/// @note 個別フィールドを返すことで ValidateAbi() がミスマッチ箇所をログに出力できる。
SANDBOXSCRIPTS_API fbzz::scene::ScriptDllAbiInfo FBZZScripts_GetAbiInfo()
{
    return fbzz::scene::GetScriptDllAbiInfo();
}

/// @note DLL に登録されているスクリプト数を返す
SANDBOXSCRIPTS_API int SandboxScripts_Count()
{
    return static_cast<int>(AllEntries().size());
}

/// @note i 番目のスクリプト型名を返す
SANDBOXSCRIPTS_API const char* SandboxScripts_TypeName(int i)
{
    const auto& entries = AllEntries();
    if (i < 0 || i >= static_cast<int>(entries.size())) return "";
    return entries[static_cast<size_t>(i)].name.c_str();
}

/// @brief EXE 側の ScriptFactory::Register を関数ポインタとして受け取り、全スクリプトを登録する。
/// @note DLL 内で ScriptFactory::Register() を直接呼ぶと DLL のレジストリコピーに登録されるため、EXE の Register 関数経由にする。
/// @note 関数名 FBZZScripts_Register はエンジン共通のエントリポイント名 (ScriptDllLoader はこの名前だけを探す)。
SANDBOXSCRIPTS_API void FBZZScripts_Register(
    void(*registerFn)(const char* typeName, std::function<std::unique_ptr<fbzz::scene::Script>()>))
{
    if (!registerFn) return;
    for (const auto& entry : AllEntries())
        registerFn(entry.name.c_str(), entry.factory);
}

}
