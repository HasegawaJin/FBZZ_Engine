// FBZZ Engine
// ScriptDllLoader.hpp | fbzz::editor
// スクリプト DLL のロード / アンロード / ホットリロード管理
//
// WHAT: SandboxScripts.dll を LoadLibrary で動的にロードし、
//       エクスポート関数 SandboxScripts_Register() 経由で ScriptFactory に登録する。
//
// WHY (ファイルロック対策):
//   Windows の LoadLibrary はロードした DLL ファイルをロックする。
//   ビルド中に上書きできないため、ロード前に _hot/ ディレクトリへ
//   タイムスタンプ付きコピーを作成し、そのコピーをロードする。
//   古いコピーは次回 Reload 時に掃除する。
//
// WHY (スクリプト破棄順序):
//   仮想デストラクタが DLL コード内にあるため、FreeLibrary 前に
//   すべての Script インスタンスを破棄しなければならない。
//   Reload() はシーンをシリアライズ → Script 全破棄 → FreeLibrary →
//   LoadLibrary → デシリアライズの順で処理する。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace fbzz::scene { class Script; }

namespace fbzz::editor {

class ScriptDllLoader {
public:
    ScriptDllLoader()  = default;
    ~ScriptDllLoader() { Unload(); }

    ScriptDllLoader(const ScriptDllLoader&)            = delete;
    ScriptDllLoader& operator=(const ScriptDllLoader&) = delete;

    // DLL を _hot/ にコピーしてロードし、ScriptFactory に登録する。
    // @return ロード成功なら true
    [[nodiscard]] bool Load(const std::filesystem::path& dllPath);

    // Script インスタンスをすべて破棄してから FreeLibrary する。
    // @param scene Script を保持するシーン。nullptr なら破棄はスキップ。
    void Unload(scene::Scene* scene = nullptr);

    // シーン状態を維持したまま DLL を差し替える (ホットリロード本体)。
    // 手順: Serialize → Script 全破棄 → FreeLibrary → Load → Deserialize
    // Play 中は false を返してリロードをスキップする。
    // @return リロード成功なら true
    [[nodiscard]] bool Reload(scene::Scene& scene, const std::filesystem::path& newDllPath);

    [[nodiscard]] bool IsLoaded()  const { return m_hDll != nullptr; }
    [[nodiscard]] const std::filesystem::path& GetDllPath() const { return m_dllPath; }

private:
    // DLL を _hot/ ディレクトリにタイムスタンプ付きでコピーして返す
    [[nodiscard]] std::filesystem::path CopyToHot(const std::filesystem::path& src) const;

    // _hot/ 内の古いコピーを削除する (現在ロード中のファイルは除く)
    void CleanHotDir() const;

    // DLL エクスポート関数を使って ScriptFactory に登録する
    void RegisterScripts();
    // Scene / Script の型レイアウトがホスト側と一致するか検証する
    [[nodiscard]] bool ValidateAbi() const;

    // シーン内の全 Script インスタンスを破棄する (FreeLibrary 前に呼ぶ)
    static void DestroyAllScripts(scene::Scene& scene);

    HMODULE               m_hDll    = nullptr;
    std::filesystem::path m_dllPath;     // 元の DLL パス (ビルド出力先)
    std::filesystem::path m_hotCopy;     // 現在ロード中のコピーパス
};

} // namespace fbzz::editor
