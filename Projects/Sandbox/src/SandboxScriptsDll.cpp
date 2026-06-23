// FBZZ Engine
// SandboxScriptsDll.cpp | sandbox
// スクリプト DLL のエントリポイント
//
// WHY (コールバック渡し設計):
//   fbzz_engine は shared runtime として EXE / Script DLL から共有される。
//   ただし Script DLL は任意のユーザーコードを後からロードする拡張境界なので、
//   登録 API は DLL 側からグローバル状態へ暗黙アクセスするより、EXE が渡す関数ポインタ経由にする。
//   これにより ScriptFactory の所有者をホスト側へ固定し、将来の外部プラグイン SDK 化でも
//   境界が明確なまま保てる。

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
// WHY: #include "Scripts/Foo.hpp" は CMakeLists の include_directories(Assets/) により
//      Assets/Scripts/Foo.hpp に解決される。
// WHY (_IMPL マクロ事前定義): 各スクリプトの inline 実装は同名 .cpp が担当する独立 TU で
//      コンパイルする。このファイルは型定義・ファクトリ用に宣言だけを取り込む。
//      Ninja はスクリプト .cpp 群と SandboxScriptsDll.cpp を並列にコンパイルできる。
#define PlayerControllerComponent_IMPL
#include "Scripts/PlayerControllerComponent.hpp"
#define TpsCameraComponent_IMPL
#include "Scripts/TpsCameraComponent.hpp"
#define SceneManagerScript_IMPL
#include "Scripts/SceneManagerScript.hpp"
// @@FBZZ_SCRIPT_INCLUDES_END

// WHY: ScriptSceneProxy::GetComponent<T>() のテンプレート定義は Scene.hpp 末尾にある。
//      スクリプトヘッダは Script.hpp しかインクルードしないため、
//      DLL エントリポイントで Scene.hpp を明示的にインクルードして
//      全 GetComponent 特殊化をこの TU でインスタンス化する。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptDllAbi.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// DLL API マクロ
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

// WHY: DLL 内に登録済みスクリプト一覧を保持することで、
//      SandboxScripts_Register() を複数回呼んでも正しく再登録できる。
// WHY: エントリは Scripts/ScriptList.inl で一元管理する。
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
SANDBOXSCRIPTS_API fbzz::scene::ScriptDllAbiInfo FBZZScripts_GetAbiInfo()
{
    return fbzz::scene::GetScriptDllAbiInfo();
}

// DLL に登録されているスクリプト数を返す
SANDBOXSCRIPTS_API int SandboxScripts_Count()
{
    return static_cast<int>(AllEntries().size());
}

// i 番目のスクリプト型名を返す
SANDBOXSCRIPTS_API const char* SandboxScripts_TypeName(int i)
{
    const auto& entries = AllEntries();
    if (i < 0 || i >= static_cast<int>(entries.size())) return "";
    return entries[static_cast<size_t>(i)].name.c_str();
}

// EXE 側の ScriptFactory::Register を関数ポインタとして受け取り、全スクリプトを登録する。
// WHY: DLL 内で ScriptFactory::Register() を直接呼ぶと DLL のレジストリコピーに登録されてしまう。
//      EXE 側の Register 関数を引数で受け取ることで EXE のレジストリへの登録を保証する。
// WHY (関数名): FBZZScripts_Register はエンジン共通のエントリポイント名。
//      ScriptDllLoader はこの名前だけを探すため、プロジェクト固有名を使わない。
SANDBOXSCRIPTS_API void FBZZScripts_Register(
    void(*registerFn)(const char* typeName, std::function<std::unique_ptr<fbzz::scene::Script>()>))
{
    if (!registerFn) return;
    for (const auto& entry : AllEntries())
        registerFn(entry.name.c_str(), entry.factory);
}

} // extern "C"
