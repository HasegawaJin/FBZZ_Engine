/// {{PROJECT_NAME}}
/// {{TARGET_NAME}}ScriptsDll.cpp | {{CPP_NAMESPACE}}
/// スクリプト DLL のエントリポイント
///
/// @note fbzz_engine は EXE / Script DLL が共有する runtime。Script DLL は任意のユーザーコードを読み込む拡張境界なので、
///       登録 API は DLL からグローバル状態へ直接アクセスさせず EXE が渡す関数ポインタ経由にし、ScriptFactory の所有権をホスト側に固定する。
/// @note スクリプト追加手順: Assets/Scripts/ に Xxx.hpp を作成 (Script 継承、TYPE_NAME 定義) → include ブロックと
///       ScriptList.inl の FBZZ_SCRIPT_ENTRY(ns, Xxx) を同期 (Editor の AssetBrowser「Create → C++ Script...」でも自動生成可)。

/// @note ScriptSceneProxy::GetComponent<T>() の定義は Scene.hpp 末尾にある。スクリプトヘッダーは Script.hpp しか
///       include しないため、DLL エントリポイントで Scene.hpp を明示 include する。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptDllAbi.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
/// @note 新方式のスクリプトは実装を inline 化したため _IMPL ガードや専用 .cpp は不要。ヘッダーを include するだけで
///       実装もこの TU に取り込まれる (複数 TU でも ODR 安全)。
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/TpsCameraComponent.hpp"
#include "Scripts/SceneManagerScript.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

/// @note DataAsset 型を DataAssetFactory へ直接自己登録する (純共有 ScriptableObject)。Script のような callback
///       渡しを要しない (エディタ専用のデータ定義のため)。リロード時は ScriptDllLoader が ClearCache/UnregisterAll した後、再登録される。
#define FBZZ_DATA_ASSET_ENTRY(ns, T) FBZZ_REGISTER_DATA_ASSET(::ns::T)
#include "Scripts/DataAssetList.inl"
#undef FBZZ_DATA_ASSET_ENTRY

#ifdef GAMESCRIPTS_EXPORTS
#  define GAMESCRIPTS_API __declspec(dllexport)
#else
#  define GAMESCRIPTS_API __declspec(dllimport)
#endif

namespace {

struct ScriptEntry {
    std::string name;
    std::function<std::unique_ptr<fbzz::scene::Script>()> factory;
};

/// @note エントリは Assets/Scripts/ScriptList.inl で一元管理する。ScriptCodeGen は ScriptList.inl だけを更新するため、このファイルのエントリを手動編集する必要はない。
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
GAMESCRIPTS_API fbzz::scene::ScriptDllAbiInfo FBZZScripts_GetAbiInfo()
{
    return fbzz::scene::GetScriptDllAbiInfo();
}

GAMESCRIPTS_API int FBZZScripts_Count()
{
    return static_cast<int>(AllEntries().size());
}

GAMESCRIPTS_API const char* FBZZScripts_TypeName(int i)
{
    const auto& entries = AllEntries();
    if (i < 0 || i >= static_cast<int>(entries.size())) return "";
    return entries[static_cast<size_t>(i)].name.c_str();
}

/// @note FBZZScripts_Register はエンジン共通のエントリポイント名。ScriptDllLoader はこの名前だけを探すため、プロジェクト固有名を使わない。
GAMESCRIPTS_API void FBZZScripts_Register(
    void(*registerFn)(const char* typeName, std::function<std::unique_ptr<fbzz::scene::Script>()>))
{
    if (!registerFn) return;
    for (const auto& entry : AllEntries())
        registerFn(entry.name.c_str(), entry.factory);
}

}
