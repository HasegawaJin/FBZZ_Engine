// {{PROJECT_NAME}}
// {{TARGET_NAME}}ScriptsDll.cpp | {{CPP_NAMESPACE}}
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
//   1. Src/Scripts/ に Xxx.hpp を作成 (Script 継承、TYPE_NAME 定義)
//   2. @@FBZZ_SCRIPT_INCLUDES_BEGIN の直後に #include "Scripts/Xxx.hpp" を追加
//   3. @@FBZZ_SCRIPT_ENTRIES_BEGIN の直後に エントリを追加
//   → Editor の AssetBrowser から "Create → C++ Script..." でも自動生成できる

// @@FBZZ_SCRIPT_INCLUDES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
// @@FBZZ_SCRIPT_INCLUDES_END

// WHY: ScriptSceneProxy::GetComponent<T>() のテンプレート定義は Scene.hpp 末尾にある。
//      スクリプトヘッダは Script.hpp しかインクルードしないため、
//      DLL エントリポイントで Scene.hpp を明示的にインクルードする。
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

const std::vector<ScriptEntry>& AllEntries()
{
    // @@FBZZ_SCRIPT_ENTRIES_BEGIN — ScriptCodeGen が自動挿入するため編集しないこと
    static const std::vector<ScriptEntry> entries = {
    };
    // @@FBZZ_SCRIPT_ENTRIES_END
    return entries;
}

} // namespace

extern "C" {

GAMESCRIPTS_API int SandboxScripts_Count()
{
    return static_cast<int>(AllEntries().size());
}

GAMESCRIPTS_API const char* SandboxScripts_TypeName(int i)
{
    const auto& entries = AllEntries();
    if (i < 0 || i >= static_cast<int>(entries.size())) return "";
    return entries[static_cast<size_t>(i)].name.c_str();
}

GAMESCRIPTS_API void SandboxScripts_Register(
    void(*registerFn)(const char* typeName, std::function<std::unique_ptr<fbzz::scene::Script>()>))
{
    if (!registerFn) return;
    for (const auto& entry : AllEntries())
        registerFn(entry.name.c_str(), entry.factory);
}

} // extern "C"
