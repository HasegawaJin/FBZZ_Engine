// DemoGame
// DemoGameScriptsDll.cpp | demogame
// スクリプト DLL のエントリポイント
//
// WHY (コールバック渡し設計):
//   fbzz_engine は shared runtime として EXE / Script DLL から共有される。
//   ただし Script DLL は任意のユーザーコードを後からロードする拡張境界なので、
//   登録 API は DLL 側からグローバル状態へ暗黙アクセスするより、EXE が渡す関数ポインタ経由にする。
//   これにより ScriptFactory の所有者をホスト側へ固定し、将来の外部プラグイン SDK 化でも
//   境界が明確なまま保てる。
//
// スクリプト追加手順:
//   1. Assets/Scripts/ に Xxx.hpp を作成 (Script 継承、TYPE_NAME 定義)
//   2. @@FBZZ_SCRIPT_INCLUDES_BEGIN の直後に #include "Scripts/Xxx.hpp" を追加
//   3. Assets/Scripts/ScriptList.inl に FBZZ_SCRIPT_ENTRY(ns, Xxx) を追加
//   → Editor の AssetBrowser から "Create → C++ Script..." でも自動生成できる

// WHY: ScriptSceneProxy::GetComponent<T>() のテンプレート定義は Scene.hpp 末尾にある。
//      スクリプトヘッダは Script.hpp しかインクルードしないため、
//      DLL エントリポイントで Scene.hpp を明示的にインクルードする。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptDllAbi.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
// WHY: PlayerIKComponent は scene.GetScript<PlayerControllerComponent>() を使うため
//      PlayerControllerComponent の完全型が必要。先に宣言を取り込んでから
//      PlayerIKComponent の IMPL ブロックを走らせることで解決する。
#define PlayerControllerComponent_IMPL
#include "Scripts/PlayerControllerComponent.hpp"
#include "Scripts/PlayerIKComponent.hpp"
#define TpsCameraComponent_IMPL
#include "Scripts/TpsCameraComponent.hpp"
#define SceneManagerScript_IMPL
#include "Scripts/SceneManagerScript.hpp"
#include "Scripts/SwordParticleComponent.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

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

// WHY: エントリは Assets/Scripts/ScriptList.inl で一元管理する。
//      ScriptCodeGen は ScriptList.inl だけを更新するため、このファイルのエントリを手動編集する必要はない。
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

// ホストと DLL の型レイアウトが一致する場合だけ ScriptFactory 登録を許可する。
// WHY: 個別フィールドを返すことで ValidateAbi() がミスマッチ箇所をログに出力できる。
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

// WHY: FBZZScripts_Register はエンジン共通のエントリポイント名。
//      ScriptDllLoader はこの名前だけを探すため、プロジェクト固有名を使わない。
GAMESCRIPTS_API void FBZZScripts_Register(
    void(*registerFn)(const char* typeName, std::function<std::unique_ptr<fbzz::scene::Script>()>))
{
    if (!registerFn) return;
    for (const auto& entry : AllEntries())
        registerFn(entry.name.c_str(), entry.factory);
}

} // extern "C"
